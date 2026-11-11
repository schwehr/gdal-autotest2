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

// Unit tests for libtiff/tif_compress.c (default compression hooks, scheme
// setup, max compression ratio, and codec registry management).
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_compress.c

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

using ::testing::ElementsAre;

constexpr uint16_t kUnknownScheme = 65534;
constexpr uint16_t kCustomSchemeA = 65501;
constexpr uint16_t kCustomSchemeB = 65502;
constexpr uint16_t kCustomSchemeC = 65503;

struct GlobalErrorRecord {
  bool is_ext = false;
  thandle_t handle = nullptr;
  std::string module;
  std::string message;
};

struct GlobalErrorCaptureState {
  std::vector<GlobalErrorRecord> errors;
};

GlobalErrorCaptureState* g_error_capture = nullptr;

class ScopedGlobalErrorCaptureBinding {
 public:
  explicit ScopedGlobalErrorCaptureBinding(GlobalErrorCaptureState* state)
      : prev_(g_error_capture) {
    g_error_capture = state;
  }

  ~ScopedGlobalErrorCaptureBinding() { g_error_capture = prev_; }

  ScopedGlobalErrorCaptureBinding(const ScopedGlobalErrorCaptureBinding&) =
      delete;
  ScopedGlobalErrorCaptureBinding& operator=(
      const ScopedGlobalErrorCaptureBinding&) = delete;

 private:
  GlobalErrorCaptureState* prev_;
};

std::string FormatVa(const char* fmt, va_list ap) {
  if (fmt == nullptr) return {};
  va_list copy;
  va_copy(copy, ap);
  const int len = std::vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  if (len < 0) return {};
  std::string out(static_cast<size_t>(len) + 1, '\0');
  std::vsnprintf(out.data(), out.size(), fmt, ap);
  out.resize(static_cast<size_t>(len));
  return out;
}

void CaptureGlobalError(const char* module, const char* fmt, va_list ap) {
  if (g_error_capture == nullptr) return;
  g_error_capture->errors.push_back(GlobalErrorRecord{
      .is_ext = false,
      .handle = nullptr,
      .module = module != nullptr ? module : "",
      .message = FormatVa(fmt, ap),
  });
}

void CaptureGlobalExtError(thandle_t handle, const char* module,
                           const char* fmt, va_list ap) {
  if (g_error_capture == nullptr) return;
  g_error_capture->errors.push_back(GlobalErrorRecord{
      .is_ext = true,
      .handle = handle,
      .module = module != nullptr ? module : "",
      .message = FormatVa(fmt, ap),
  });
}

bool HasDiagnosticContaining(const DiagnosticRecorder& recorder,
                             std::string_view substr) {
  const std::vector<DiagnosticRecord> errors = recorder.errors();
  return std::any_of(errors.begin(), errors.end(),
                     [substr](const DiagnosticRecord& rec) {
                       return rec.message.find(substr) != std::string::npos;
                     });
}

void ConfigureStrippedWriteTags(TIFF* tif, uint32_t width, uint32_t height,
                                uint16_t bits_per_sample,
                                uint16_t samples_per_pixel,
                                uint32_t rows_per_strip, uint16_t planar_config,
                                uint16_t compression) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, compression), 1);
}

void ConfigureTiledWriteTags(TIFF* tif, uint32_t width, uint32_t height,
                             uint32_t tile_width, uint32_t tile_height,
                             uint16_t bits_per_sample, uint16_t compression) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILEWIDTH, tile_width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILELENGTH, tile_height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, compression), 1);
}

std::vector<uint8_t> BuildMinimalStrippedTiff(uint32_t width, uint32_t height,
                                              uint16_t bits_per_sample,
                                              uint32_t rows_per_strip,
                                              uint16_t compression,
                                              bool big_endian, bool big_tiff) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const std::array<uint8_t, 4> dummy_strip = {0x11, 0x22, 0x33, 0x44};
  const uint64_t strip_offset = builder.AppendBlob(dummy_strip);

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bits_per_sample),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      big_tiff ? builder.MakeLong8(TIFFTAG_STRIPOFFSETS, strip_offset)
               : builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                                  static_cast<uint32_t>(strip_offset)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, rows_per_strip),
      big_tiff ? builder.MakeLong8(TIFFTAG_STRIPBYTECOUNTS, dummy_strip.size())
               : builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                                  static_cast<uint32_t>(dummy_strip.size())),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
  };
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

std::vector<uint8_t> BuildMinimalTwoStripTiff(uint32_t width, uint32_t height,
                                              uint16_t bits_per_sample,
                                              uint32_t rows_per_strip,
                                              uint16_t compression,
                                              bool big_endian, bool big_tiff) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const std::array<uint8_t, 4> dummy_strip = {0x11, 0x22, 0x33, 0x44};
  const uint64_t off0 = builder.AppendBlob(dummy_strip);
  const uint64_t off1 = builder.AppendBlob(dummy_strip);
  const std::array<uint64_t, 2> offsets64 = {off0, off1};
  const std::array<uint32_t, 2> offsets32 = {static_cast<uint32_t>(off0),
                                             static_cast<uint32_t>(off1)};
  const std::array<uint64_t, 2> counts64 = {dummy_strip.size(),
                                            dummy_strip.size()};
  const std::array<uint32_t, 2> counts32 = {
      static_cast<uint32_t>(dummy_strip.size()),
      static_cast<uint32_t>(dummy_strip.size())};

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bits_per_sample),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      big_tiff ? builder.MakeLong8Array(TIFFTAG_STRIPOFFSETS, offsets64)
               : builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offsets32),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, rows_per_strip),
      big_tiff ? builder.MakeLong8Array(TIFFTAG_STRIPBYTECOUNTS, counts64)
               : builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, counts32),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
  };
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

std::vector<uint8_t> BuildMinimalSeparatePlaneTiff(
    uint32_t width, uint32_t height, uint16_t bits_per_sample,
    uint16_t samples_per_pixel, uint16_t compression, bool big_endian,
    bool big_tiff) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const std::array<uint8_t, 4> dummy_strip = {0x11, 0x22, 0x33, 0x44};
  std::vector<uint64_t> offsets64(samples_per_pixel);
  std::vector<uint32_t> offsets32(samples_per_pixel);
  std::vector<uint64_t> counts64(samples_per_pixel, dummy_strip.size());
  std::vector<uint32_t> counts32(samples_per_pixel,
                                 static_cast<uint32_t>(dummy_strip.size()));
  for (uint16_t s = 0; s < samples_per_pixel; ++s) {
    const uint64_t off = builder.AppendBlob(dummy_strip);
    offsets64[s] = off;
    offsets32[s] = static_cast<uint32_t>(off);
  }

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bits_per_sample),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      big_tiff ? builder.MakeLong8Array(TIFFTAG_STRIPOFFSETS, offsets64)
               : builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offsets32),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, height),
      big_tiff ? builder.MakeLong8Array(TIFFTAG_STRIPBYTECOUNTS, counts64)
               : builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, counts32),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE),
  };
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

std::vector<uint8_t> BuildMinimalTiledTiff(uint32_t width, uint32_t height,
                                           uint32_t tile_width,
                                           uint32_t tile_height,
                                           uint16_t bits_per_sample,
                                           uint16_t compression,
                                           bool big_endian, bool big_tiff) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const std::array<uint8_t, 4> dummy_tile = {0xAA, 0xBB, 0xCC, 0xDD};
  const uint64_t tile_offset = builder.AppendBlob(dummy_tile);

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bits_per_sample),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
      builder.MakeLong(TIFFTAG_TILEWIDTH, tile_width),
      builder.MakeLong(TIFFTAG_TILELENGTH, tile_height),
      big_tiff ? builder.MakeLong8(TIFFTAG_TILEOFFSETS, tile_offset)
               : builder.MakeLong(TIFFTAG_TILEOFFSETS,
                                  static_cast<uint32_t>(tile_offset)),
      big_tiff ? builder.MakeLong8(TIFFTAG_TILEBYTECOUNTS, dummy_tile.size())
               : builder.MakeLong(TIFFTAG_TILEBYTECOUNTS,
                                  static_cast<uint32_t>(dummy_tile.size())),
  };
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

int InitDefaultHooksOnly(TIFF* /*tif*/, int /*scheme*/) { return 1; }

int InitAlwaysFail(TIFF* /*tif*/, int /*scheme*/) { return 0; }

int DummyStripEncodeOk(TIFF* tif, uint8_t* pp, tmsize_t cc, uint16_t /*s*/) {
  if (tif->tif_rawcc + cc <= tif->tif_rawdatasize) {
    std::memcpy(tif->tif_rawcp, pp, static_cast<size_t>(cc));
    tif->tif_rawcp += cc;
    tif->tif_rawcc += cc;
    return 1;
  }
  return 0;
}

int InitCustomStripEncoderWithDefaultPrePost(TIFF* tif, int /*scheme*/) {
  tif->tif_encodestrip = &DummyStripEncodeOk;
  return 1;
}

struct InitTracker {
  int calls = 0;
  int last_scheme = 0;
};

InitTracker* g_init_tracker = nullptr;

int InitTrackingCodec(TIFF* /*tif*/, int scheme) {
  if (g_init_tracker != nullptr) {
    ++g_init_tracker->calls;
    g_init_tracker->last_scheme = scheme;
  }
  return 1;
}

TEST(TifCompressTest, SetDefaultCompressionStateResetsHooksAndFlags) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  // Switch to COMPRESSION_NONE first so hooks and max ratio differ from the
  // default state, and also set TIFF_NOBITREV | TIFF_NOREADRAW plus an
  // unrelated flag that must be preserved.
  ASSERT_EQ(TIFFSetCompressionScheme(tif.get(), COMPRESSION_NONE), 1);
  EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 1U);

  const uint32_t prev_flags = tif->tif_flags;
  tif->tif_flags |= (TIFF_NOBITREV | TIFF_NOREADRAW | TIFF_SWAB);

  _TIFFSetDefaultCompressionState(tif.get());

  EXPECT_NE(tif->tif_fixuptags, nullptr);
  EXPECT_EQ((*tif->tif_fixuptags)(tif.get()), 1);
  EXPECT_EQ(tif->tif_decodestatus, TRUE);
  EXPECT_NE(tif->tif_setupdecode, nullptr);
  EXPECT_EQ((*tif->tif_setupdecode)(tif.get()), 1);
  EXPECT_EQ(tif->tif_predecode, &_TIFFNoPreCode);
  EXPECT_EQ(tif->tif_decoderow, &_TIFFNoRowDecode);
  EXPECT_EQ(tif->tif_decodestrip, &_TIFFNoStripDecode);
  EXPECT_EQ(tif->tif_decodetile, &_TIFFNoTileDecode);
  EXPECT_EQ(tif->tif_encodestatus, TRUE);
  EXPECT_NE(tif->tif_setupencode, nullptr);
  EXPECT_EQ((*tif->tif_setupencode)(tif.get()), 1);
  EXPECT_EQ(tif->tif_preencode, &_TIFFNoPreCode);
  EXPECT_NE(tif->tif_postencode, nullptr);
  EXPECT_EQ((*tif->tif_postencode)(tif.get()), 1);
  EXPECT_EQ(tif->tif_encoderow, &_TIFFNoRowEncode);
  EXPECT_EQ(tif->tif_encodestrip, &_TIFFNoStripEncode);
  EXPECT_EQ(tif->tif_encodetile, &_TIFFNoTileEncode);
  EXPECT_NE(tif->tif_close, nullptr);
  EXPECT_NE(tif->tif_cleanup, nullptr);
  (*tif->tif_close)(tif.get());
  (*tif->tif_cleanup)(tif.get());
  EXPECT_EQ(tif->tif_seek, &_TIFFNoSeek);
  EXPECT_EQ(tif->tif_defstripsize, &_TIFFDefaultStripSize);
  EXPECT_EQ(tif->tif_deftilesize, &_TIFFDefaultTileSize);
  EXPECT_NE(tif->tif_getmaxcompressionratio, nullptr);
  EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 0U);
  EXPECT_EQ(tif->tif_flags & (TIFF_NOBITREV | TIFF_NOREADRAW), 0U);
  EXPECT_EQ(tif->tif_flags & TIFF_SWAB, TIFF_SWAB);

  tif->tif_flags = prev_flags;
}

TEST(TifCompressTest, SetCompressionSchemeHandlesKnownUnknownAndFailingInit) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  // COMPRESSION_NONE sets _TIFFGetMaxCompressionRatioOne (-> 1) and DumpMode.
  EXPECT_EQ(TIFFSetCompressionScheme(tif.get(), COMPRESSION_NONE), 1);
  EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 1U);

  // Unknown scheme resets state and leaves _TIFFDefaultGetMaxCompressionRatio.
  EXPECT_EQ(TIFFSetCompressionScheme(tif.get(), kUnknownScheme), 1);
  EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 0U);
  EXPECT_EQ(tif->tif_encoderow, &_TIFFNoRowEncode);
  EXPECT_EQ(tif->tif_decoderow, &_TIFFNoRowDecode);

  // Null tif_getmaxcompressionratio pointer safely falls back to 0.
  const auto saved_fn = tif->tif_getmaxcompressionratio;
  tif->tif_getmaxcompressionratio = nullptr;
  EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 0U);
  tif->tif_getmaxcompressionratio = saved_fn;

  // Custom codec whose init succeeds vs fails.
  InitTracker tracker;
  g_init_tracker = &tracker;
  auto clear_tracker = absl::MakeCleanup([] { g_init_tracker = nullptr; });

  ScopedRegisteredCodec ok_codec(kCustomSchemeA, "CustomTrackOk",
                                 &InitTrackingCodec);
  ASSERT_NE(ok_codec.get(), nullptr);
  EXPECT_EQ(TIFFSetCompressionScheme(tif.get(), kCustomSchemeA), 1);
  EXPECT_EQ(tracker.calls, 1);
  EXPECT_EQ(tracker.last_scheme, kCustomSchemeA);

  ScopedRegisteredCodec fail_codec(kCustomSchemeB, "CustomFailInit",
                                   &InitAlwaysFail);
  ASSERT_NE(fail_codec.get(), nullptr);
  EXPECT_EQ(TIFFSetCompressionScheme(tif.get(), kCustomSchemeB), 0);

  // TIFFSetField(TIFFTAG_COMPRESSION, ...) propagates init failure and does not
  // update td_compression.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION,
                         static_cast<uint16_t>(COMPRESSION_NONE)),
            1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, kCustomSchemeB), 0);
  uint16_t compression = 0;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_COMPRESSION, &compression), 1);
  EXPECT_EQ(compression, COMPRESSION_NONE);
}

TEST(TifCompressTest, MaxCompressionRatioRejectsImplausibleUncompressedStrip) {
  struct LargeStripCase {
    bool big_endian;
    bool big_tiff;
  };
  constexpr auto kCases = std::to_array<LargeStripCase>({
      {.big_endian = false, .big_tiff = false},
      {.big_endian = true, .big_tiff = false},
      {.big_endian = false, .big_tiff = true},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.big_endian ? "BE" : "LE");
    SCOPED_TRACE(tc.big_tiff ? "BigTIFF" : "ClassicTIFF");

    // Two strips of 100,000 * 1,250 = 125 MB each (> 100 MiB threshold), with
    // only 4-byte strip payloads (2 strips avoids single-strip
    // ByteCountLooksBad estimation in tif_dirread.c). Opening with "rc"
    // disables strip chopping so TIFFFillStrip queries
    // TIFFGetMaxCompressionRatio (1 for COMPRESSION_NONE) and rejects early.
    MemoryTiffStream stream(BuildMinimalTwoStripTiff(
        /*width=*/100000U, /*height=*/2500U, /*bits_per_sample=*/8,
        /*rows_per_strip=*/1250U, COMPRESSION_NONE, tc.big_endian,
        tc.big_tiff));
    UniqueTiff tif = stream.Open("rc");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 1U);

    std::array<uint8_t, 4> small_buf{};
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, small_buf.data(),
                                   static_cast<tmsize_t>(small_buf.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics(),
                                        "Likely invalid strip byte count"));

    // Same heuristic via TIFFFillTile with 16384x8192 (128 MiB) tile.
    MemoryTiffStream tile_stream(BuildMinimalTiledTiff(
        /*width=*/16384U, /*height=*/8192U, /*tile_width=*/16384U,
        /*tile_height=*/8192U, /*bits_per_sample=*/8, COMPRESSION_NONE,
        tc.big_endian, tc.big_tiff));
    UniqueTiff tile_tif = tile_stream.Open("r");
    ASSERT_NE(tile_tif, nullptr);
    EXPECT_EQ(TIFFGetMaxCompressionRatio(tile_tif.get()), 1U);
    EXPECT_EQ(TIFFReadEncodedTile(tile_tif.get(), 0, small_buf.data(),
                                  static_cast<tmsize_t>(small_buf.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(tile_stream.diagnostics(),
                                        "Likely invalid tile byte count"));
  }
}

TEST(TifCompressTest, NoEncodeHooksReportKnownAndUnknownSchemes) {
  for (const char* mode : {"wl", "wb", "w8"}) {
    SCOPED_TRACE(mode);
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open(mode);
    ASSERT_NE(tif, nullptr);

    ConfigureStrippedWriteTags(tif.get(), /*width=*/4U, /*height=*/4U,
                               /*bits_per_sample=*/8,
                               /*samples_per_pixel=*/1, /*rows_per_strip=*/4U,
                               PLANARCONFIG_CONTIG, kUnknownScheme);

    std::array<uint8_t, 4> row = {1, 2, 3, 4};
    EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(),
        "Compression scheme 65534 scanline encoding is not implemented"));

    stream.diagnostics().Clear();
    EXPECT_EQ(_TIFFNoRowEncode(tif.get(), row.data(),
                               static_cast<tmsize_t>(row.size()), 0),
              -1);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(),
        "Compression scheme 65534 scanline encoding is not implemented"));

    stream.diagnostics().Clear();
    std::array<uint8_t, 16> strip{};
    EXPECT_EQ(_TIFFNoStripEncode(tif.get(), strip.data(),
                                 static_cast<tmsize_t>(strip.size()), 0),
              -1);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(),
        "Compression scheme 65534 strip encoding is not implemented"));

    stream.diagnostics().Clear();
    EXPECT_EQ(_TIFFNoTileEncode(tif.get(), strip.data(),
                                static_cast<tmsize_t>(strip.size()), 0),
              -1);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(),
        "Compression scheme 65534 tile encoding is not implemented"));
  }

  // Known codec with no encoder (ThunderScan leaves _TIFFNo*Encode).
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ConfigureStrippedWriteTags(tif.get(), /*width=*/8U, /*height=*/2U,
                               /*bits_per_sample=*/4,
                               /*samples_per_pixel=*/1, /*rows_per_strip=*/2U,
                               PLANARCONFIG_CONTIG, COMPRESSION_THUNDERSCAN);
    std::array<uint8_t, 4> row{};
    EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(),
        "ThunderScan scanline encoding is not implemented"));
  }

  // Planar-separate 16-bit write passes non-zero sample `s` to
  // _TIFFNoRowEncode.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ConfigureStrippedWriteTags(tif.get(), /*width=*/2U, /*height=*/1U,
                               /*bits_per_sample=*/16,
                               /*samples_per_pixel=*/3, /*rows_per_strip=*/1U,
                               PLANARCONFIG_SEPARATE, kUnknownScheme);
    std::array<uint16_t, 2> row16 = {0x1234, 0x5678};
    EXPECT_EQ(TIFFWriteScanline(tif.get(), row16.data(), 0, /*sample=*/2), -1);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(),
        "Compression scheme 65534 scanline encoding is not implemented"));
  }

  // Known custom codec leaving default _TIFFNo*Encode hooks for row/strip/tile.
  // Note: TIFFWriteEncodedStrip/TIFFWriteEncodedTile in tif_write.c check
  // `!(*tif->tif_encodestrip)(...)` against -1 (false), so we verify the
  // installed strip/tile encode hooks directly rather than cementing that bug.
  {
    ScopedRegisteredCodec no_encode_codec(kCustomSchemeA, "CustomNoEncode",
                                          &InitDefaultHooksOnly);
    ASSERT_NE(no_encode_codec.get(), nullptr);

    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ConfigureTiledWriteTags(tif.get(), /*width=*/16U, /*height=*/16U,
                            /*tile_width=*/16U, /*tile_height=*/16U,
                            /*bits_per_sample=*/8, kCustomSchemeA);

    std::array<uint8_t, 16> buf{};
    EXPECT_EQ((*tif->tif_encoderow)(tif.get(), buf.data(), 16, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(),
        "CustomNoEncode scanline encoding is not implemented"));

    stream.diagnostics().Clear();
    EXPECT_EQ((*tif->tif_encodestrip)(tif.get(), buf.data(), 16, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(),
        "CustomNoEncode strip encoding is not implemented"));

    stream.diagnostics().Clear();
    EXPECT_EQ((*tif->tif_encodetile)(tif.get(), buf.data(), 16, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(),
        "CustomNoEncode tile encoding is not implemented"));
  }
}

TEST(TifCompressTest, NoDecodeHooksAcrossScanlineStripAndTile) {
  struct StreamVariant {
    bool big_endian;
    bool big_tiff;
  };
  constexpr auto kVariants = std::to_array<StreamVariant>({
      {.big_endian = false, .big_tiff = false},
      {.big_endian = true, .big_tiff = false},
      {.big_endian = false, .big_tiff = true},
      {.big_endian = true, .big_tiff = true},
  });

  // Unknown scheme (TIFFFindCODEC returns nullptr) across stripped and tiled.
  for (const auto& v : kVariants) {
    SCOPED_TRACE(v.big_endian ? "BE" : "LE");
    SCOPED_TRACE(v.big_tiff ? "BigTIFF" : "ClassicTIFF");

    {
      MemoryTiffStream stream(BuildMinimalStrippedTiff(
          /*width=*/4U, /*height=*/4U, /*bits_per_sample=*/8,
          /*rows_per_strip=*/4U, kUnknownScheme, v.big_endian, v.big_tiff));
      UniqueTiff tif = stream.Open("r");
      ASSERT_NE(tif, nullptr);
      EXPECT_EQ(stream.diagnostics().error_count(), 0U);

      std::array<uint8_t, 4> row{};
      EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), -1);
      EXPECT_TRUE(HasDiagnosticContaining(
          stream.diagnostics(),
          "Compression scheme 65534 scanline decoding is not implemented"));

      stream.diagnostics().Clear();
      std::array<uint8_t, 16> strip{};
      EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, strip.data(),
                                     static_cast<tmsize_t>(strip.size())),
                static_cast<tmsize_t>(-1));
      EXPECT_TRUE(HasDiagnosticContaining(
          stream.diagnostics(),
          "Compression scheme 65534 strip decoding is not implemented"));
    }

    {
      MemoryTiffStream stream(BuildMinimalTiledTiff(
          /*width=*/16U, /*height=*/16U, /*tile_width=*/16U,
          /*tile_height=*/16U, /*bits_per_sample=*/8, kUnknownScheme,
          v.big_endian, v.big_tiff));
      UniqueTiff tif = stream.Open("r");
      ASSERT_NE(tif, nullptr);
      EXPECT_EQ(stream.diagnostics().error_count(), 0U);

      std::array<uint8_t, 256> tile{};
      EXPECT_EQ(TIFFReadEncodedTile(tif.get(), 0, tile.data(),
                                    static_cast<tmsize_t>(tile.size())),
                static_cast<tmsize_t>(-1));
      EXPECT_TRUE(HasDiagnosticContaining(
          stream.diagnostics(),
          "Compression scheme 65534 tile decoding is not implemented"));

      stream.diagnostics().Clear();
      EXPECT_EQ(TIFFReadTile(tif.get(), tile.data(), 0, 0, 0, 0),
                static_cast<tmsize_t>(-1));
      EXPECT_TRUE(HasDiagnosticContaining(
          stream.diagnostics(),
          "Compression scheme 65534 tile decoding is not implemented"));
    }
  }

  // Known builtin codec without tile decode (ThunderScan with 4-bit pixels).
  {
    MemoryTiffStream stream(BuildMinimalTiledTiff(
        /*width=*/16U, /*height=*/16U, /*tile_width=*/16U,
        /*tile_height=*/16U, /*bits_per_sample=*/4, COMPRESSION_THUNDERSCAN,
        /*big_endian=*/false, /*big_tiff=*/false));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);

    std::array<uint8_t, 128> tile{};
    EXPECT_EQ(TIFFReadEncodedTile(tif.get(), 0, tile.data(),
                                  static_cast<tmsize_t>(tile.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(), "ThunderScan tile decoding is not implemented"));
  }

  // Known custom codec leaving default _TIFFNo*Decode hooks.
  {
    ScopedRegisteredCodec no_decode_codec(kCustomSchemeA, "CustomNoDecode",
                                          &InitDefaultHooksOnly);
    ASSERT_NE(no_decode_codec.get(), nullptr);

    MemoryTiffStream stream(BuildMinimalStrippedTiff(
        /*width=*/4U, /*height=*/4U, /*bits_per_sample=*/8,
        /*rows_per_strip=*/4U, kCustomSchemeA, /*big_endian=*/false,
        /*big_tiff=*/false));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);

    std::array<uint8_t, 4> row{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(),
        "CustomNoDecode scanline decoding is not implemented"));

    stream.diagnostics().Clear();
    std::array<uint8_t, 16> strip{};
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, strip.data(),
                                   static_cast<tmsize_t>(strip.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(),
        "CustomNoDecode strip decoding is not implemented"));

    stream.diagnostics().Clear();
    EXPECT_EQ(_TIFFNoRowDecode(tif.get(), row.data(), 4, 0), 0);
    EXPECT_EQ(_TIFFNoStripDecode(tif.get(), strip.data(), 16, 0), 0);
    EXPECT_EQ(_TIFFNoTileDecode(tif.get(), strip.data(), 16, 0), 0);
  }

  // Planar-separate 16-bit read passes non-zero sample `s` to _TIFFNoPreCode,
  // _TIFFNoRowDecode, and _TIFFNoStripDecode.
  {
    MemoryTiffStream stream(BuildMinimalSeparatePlaneTiff(
        /*width=*/2U, /*height=*/1U, /*bits_per_sample=*/16,
        /*samples_per_pixel=*/3, kUnknownScheme, /*big_endian=*/false,
        /*big_tiff=*/false));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    std::array<uint16_t, 2> row16{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), row16.data(), 0, /*sample=*/2), -1);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(),
        "Compression scheme 65534 scanline decoding is not implemented"));

    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), /*strip=*/2, row16.data(), 4),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(),
        "Compression scheme 65534 strip decoding is not implemented"));
  }
}

TEST(TifCompressTest, NoSeekRejectsRandomAccessOnReadAndWrite) {
  // PackBits does not override tif_seek, so it retains _TIFFNoSeek.
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureStrippedWriteTags(writer.get(), /*width=*/4U, /*height=*/4U,
                               /*bits_per_sample=*/8,
                               /*samples_per_pixel=*/1, /*rows_per_strip=*/4U,
                               PLANARCONFIG_CONTIG, COMPRESSION_PACKBITS);

    std::array<uint8_t, 4> row0 = {10, 20, 30, 40};
    ASSERT_EQ(TIFFWriteScanline(writer.get(), row0.data(), 0, 0), 1);

    // Skipping row 1 triggers tif_seek -> _TIFFNoSeek on write.
    std::array<uint8_t, 4> row2 = {50, 60, 70, 80};
    EXPECT_EQ(TIFFWriteScanline(writer.get(), row2.data(), 2, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics(),
        "Compression algorithm does not support random access"));
  }

  // Write a valid 4-row single-strip PackBits TIFF, then test non-sequential
  // read.
  MemoryTiffStream read_stream;
  {
    UniqueTiff writer = read_stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureStrippedWriteTags(writer.get(), /*width=*/4U, /*height=*/4U,
                               /*bits_per_sample=*/8,
                               /*samples_per_pixel=*/1, /*rows_per_strip=*/4U,
                               PLANARCONFIG_CONTIG, COMPRESSION_PACKBITS);
    for (uint32_t r = 0; r < 4; ++r) {
      std::array<uint8_t, 4> row = {
          static_cast<uint8_t>(r), static_cast<uint8_t>(r + 1),
          static_cast<uint8_t>(r + 2), static_cast<uint8_t>(r + 3)};
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), r, 0), 1);
    }
  }

  UniqueTiff reader = read_stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::array<uint8_t, 4> out{};
  ASSERT_EQ(TIFFReadScanline(reader.get(), out.data(), 0, 0), 1);
  EXPECT_THAT(out, ElementsAre(0, 1, 2, 3));

  // Skipping row 1 triggers tif_seek -> _TIFFNoSeek on read.
  EXPECT_EQ(TIFFReadScanline(reader.get(), out.data(), 2, 0), -1);
  EXPECT_TRUE(HasDiagnosticContaining(
      read_stream.diagnostics(),
      "Compression algorithm does not support random access"));

  read_stream.diagnostics().Clear();
  EXPECT_EQ(_TIFFNoSeek(reader.get(), 1), 0);
  EXPECT_TRUE(HasDiagnosticContaining(
      read_stream.diagnostics(),
      "Compression algorithm does not support random access"));
}

TEST(TifCompressTest, NoPreCodeAndDefaultLifecycleHooksOperateCleanly) {
  // Custom codec that uses default _TIFFNoPreCode and _TIFFtrue postencode
  // during TIFFWriteEncodedStrip.
  ScopedRegisteredCodec strip_codec(kCustomSchemeA, "CustomPrePost",
                                    &InitCustomStripEncoderWithDefaultPrePost);
  ASSERT_NE(strip_codec.get(), nullptr);

  MemoryTiffStream stream;
  {
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ConfigureStrippedWriteTags(tif.get(), /*width=*/4U, /*height=*/2U,
                               /*bits_per_sample=*/8,
                               /*samples_per_pixel=*/1, /*rows_per_strip=*/2U,
                               PLANARCONFIG_CONTIG, kCustomSchemeA);

    EXPECT_EQ(_TIFFNoPreCode(tif.get(), 0), 1);
    EXPECT_EQ(tif->tif_preencode, &_TIFFNoPreCode);
    EXPECT_EQ(tif->tif_predecode, &_TIFFNoPreCode);

    // Switching compression scheme exercises tif_cleanup (_TIFFvoid).
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION,
                           static_cast<uint16_t>(COMPRESSION_NONE)),
              1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, kCustomSchemeA), 1);

    std::array<uint8_t, 8> strip = {1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, strip.data(),
                                    static_cast<tmsize_t>(strip.size())),
              static_cast<tmsize_t>(strip.size()));
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }

  // Round-trip uncompressed write/read to verify default _TIFFNoPreCode,
  // _TIFFtrue, and _TIFFvoid lifecycle hooks preserve pixel data.
  MemoryTiffStream roundtrip_stream;
  {
    UniqueTiff writer = roundtrip_stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureStrippedWriteTags(writer.get(), /*width=*/4U, /*height=*/2U,
                               /*bits_per_sample=*/8,
                               /*samples_per_pixel=*/1, /*rows_per_strip=*/2U,
                               PLANARCONFIG_CONTIG, COMPRESSION_NONE);
    std::array<uint8_t, 4> row0 = {0x10, 0x20, 0x30, 0x40};
    std::array<uint8_t, 4> row1 = {0x50, 0x60, 0x70, 0x80};
    ASSERT_EQ(TIFFWriteScanline(writer.get(), row0.data(), 0, 0), 1);
    ASSERT_EQ(TIFFWriteScanline(writer.get(), row1.data(), 1, 0), 1);
  }
  {
    UniqueTiff reader = roundtrip_stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 4> out{};
    ASSERT_EQ(TIFFReadScanline(reader.get(), out.data(), 0, 0), 1);
    EXPECT_THAT(out, ElementsAre(0x10, 0x20, 0x30, 0x40));
    ASSERT_EQ(TIFFReadScanline(reader.get(), out.data(), 1, 0), 1);
    EXPECT_THAT(out, ElementsAre(0x50, 0x60, 0x70, 0x80));
    EXPECT_EQ(roundtrip_stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifCompressTest, FindRegisterAndUnregisterCodecLifecycle) {
  // Builtin lookup hits _TIFFBuiltinCODECS; unknown schemes return nullptr.
  const TIFFCodec* none_codec = TIFFFindCODEC(COMPRESSION_NONE);
  ASSERT_NE(none_codec, nullptr);
  EXPECT_EQ(std::string_view(none_codec->name), "None");
  EXPECT_EQ(none_codec->scheme, COMPRESSION_NONE);
  EXPECT_EQ(TIFFFindCODEC(kUnknownScheme), nullptr);
  EXPECT_EQ(TIFFFindCODEC(0), nullptr);

  // Register multiple codecs, ensuring name string is copied by
  // TIFFRegisterCODEC rather than aliasing the caller buffer.
  std::string mutable_name = "TempOwnedNameA";
  ScopedRegisteredCodec reg_a(kCustomSchemeA, mutable_name.c_str(),
                              &InitDefaultHooksOnly);
  mutable_name.assign("OverwrittenXXX");
  ASSERT_NE(reg_a.get(), nullptr);
  EXPECT_EQ(std::string_view(reg_a.get()->name), "TempOwnedNameA");
  EXPECT_EQ(TIFFFindCODEC(kCustomSchemeA), reg_a.get());

  ScopedRegisteredCodec reg_b(kCustomSchemeB, "CustomCodecB",
                              &InitDefaultHooksOnly);
  ASSERT_NE(reg_b.get(), nullptr);

  // Register a second codec with the same scheme kCustomSchemeA and an override
  // for builtin COMPRESSION_NONE; LIFO prepend means newer entries shadow older
  // and builtin entries.
  ScopedRegisteredCodec reg_a_override(kCustomSchemeA, "CustomCodecAOverride",
                                       &InitTrackingCodec);
  ASSERT_NE(reg_a_override.get(), nullptr);
  EXPECT_EQ(TIFFFindCODEC(kCustomSchemeA), reg_a_override.get());

  ScopedRegisteredCodec reg_none_override(COMPRESSION_NONE, "NoneOverride",
                                          &InitDefaultHooksOnly);
  ASSERT_NE(reg_none_override.get(), nullptr);
  EXPECT_EQ(TIFFFindCODEC(COMPRESSION_NONE), reg_none_override.get());

  // Unregister from the middle of the linked list (reg_b) and head
  // (reg_none_override), and verify remaining entries stay intact.
  TIFFUnRegisterCODEC(reg_b.release());
  EXPECT_EQ(TIFFFindCODEC(kCustomSchemeB), nullptr);
  EXPECT_EQ(TIFFFindCODEC(kCustomSchemeA), reg_a_override.get());

  TIFFUnRegisterCODEC(reg_none_override.release());
  EXPECT_EQ(TIFFFindCODEC(COMPRESSION_NONE), none_codec);

  // Removing the shadowing registration reveals the earlier kCustomSchemeA
  // registration.
  TIFFUnRegisterCODEC(reg_a_override.release());
  EXPECT_EQ(TIFFFindCODEC(kCustomSchemeA), reg_a.get());

  TIFFUnRegisterCODEC(reg_a.release());
  EXPECT_EQ(TIFFFindCODEC(kCustomSchemeA), nullptr);
}

TEST(TifCompressTest, UnregisterUnknownCodecReportsErrorWithoutLeaking) {
  GlobalErrorCaptureState capture;
  ScopedGlobalErrorCaptureBinding capture_binding(&capture);
  ScopedTiffErrorHandlers global_handlers(&CaptureGlobalError, nullptr);
  ScopedTiffErrorHandlersExt global_ext_handlers(&CaptureGlobalExtError,
                                                 nullptr);

  TIFFCodec bogus_empty = {"BogusEmpty", kCustomSchemeC, nullptr};
  TIFFUnRegisterCODEC(&bogus_empty);
  ASSERT_EQ(capture.errors.size(), 2U);
  EXPECT_EQ(capture.errors[0].module, "TIFFUnRegisterCODEC");
  EXPECT_EQ(capture.errors[0].message,
            "Cannot remove compression scheme BogusEmpty; not registered");
  EXPECT_TRUE(capture.errors[1].is_ext);
  EXPECT_EQ(capture.errors[1].handle, nullptr);
  EXPECT_EQ(capture.errors[1].module, "TIFFUnRegisterCODEC");
  EXPECT_EQ(capture.errors[1].message,
            "Cannot remove compression scheme BogusEmpty; not registered");

  // Builtin codec pointer is not in registeredCODECS and cannot be
  // unregistered.
  capture.errors.clear();
  TIFFCodec* builtin_none =
      const_cast<TIFFCodec*>(TIFFFindCODEC(COMPRESSION_NONE));
  ASSERT_NE(builtin_none, nullptr);
  TIFFUnRegisterCODEC(builtin_none);
  ASSERT_EQ(capture.errors.size(), 2U);
  EXPECT_EQ(capture.errors[0].message,
            "Cannot remove compression scheme None; not registered");

  // Non-empty registeredCODECS list: verify traversal to the end before error.
  ScopedRegisteredCodec reg_a(kCustomSchemeA, "ValidA", &InitDefaultHooksOnly);
  ScopedRegisteredCodec reg_b(kCustomSchemeB, "ValidB", &InitDefaultHooksOnly);
  ASSERT_NE(reg_a.get(), nullptr);
  ASSERT_NE(reg_b.get(), nullptr);

  capture.errors.clear();
  TIFFCodec bogus_nonempty = {"BogusNonEmpty", kCustomSchemeC, nullptr};
  TIFFUnRegisterCODEC(&bogus_nonempty);
  ASSERT_EQ(capture.errors.size(), 2U);
  EXPECT_EQ(capture.errors[0].module, "TIFFUnRegisterCODEC");
  EXPECT_EQ(capture.errors[0].message,
            "Cannot remove compression scheme BogusNonEmpty; not registered");
}

TEST(TifCompressTest, GetConfiguredCodecsIncludesRegisteredAndBuiltin) {
  {
    UniqueTiffBuffer<TIFFCodec> baseline(TIFFGetConfiguredCODECs());
    ASSERT_NE(baseline, nullptr);

    size_t count = 0;
    std::set<uint16_t> schemes;
    for (const TIFFCodec* c = baseline.get(); c->name != nullptr; ++c) {
      EXPECT_EQ(TIFFIsCODECConfigured(c->scheme), 1);
      schemes.insert(c->scheme);
      ++count;
    }
    EXPECT_GT(count, 0U);
    EXPECT_EQ(baseline.get()[count].name, nullptr);
    EXPECT_EQ(baseline.get()[count].scheme, 0);
    EXPECT_EQ(baseline.get()[count].init, nullptr);
    EXPECT_EQ(schemes.count(COMPRESSION_NONE), 1U);
    EXPECT_EQ(schemes.count(COMPRESSION_OJPEG), 0U);
    EXPECT_EQ(schemes.count(COMPRESSION_JBIG), 0U);
  }

  // Register custom codecs (including one with nullptr init) and verify they
  // appear at the front of TIFFGetConfiguredCODECs in LIFO order.
  ScopedRegisteredCodec reg_first(kCustomSchemeA, "RegFirst",
                                  &InitDefaultHooksOnly);
  ScopedRegisteredCodec reg_null(kCustomSchemeB, "RegNullInit", nullptr);
  ScopedRegisteredCodec reg_jbig(COMPRESSION_JBIG, "OverrideJBIG",
                                 &InitDefaultHooksOnly);
  ASSERT_NE(reg_first.get(), nullptr);
  ASSERT_NE(reg_null.get(), nullptr);
  ASSERT_NE(reg_jbig.get(), nullptr);

  UniqueTiffBuffer<TIFFCodec> with_custom(TIFFGetConfiguredCODECs());
  ASSERT_NE(with_custom, nullptr);

  EXPECT_EQ(std::string_view(with_custom.get()[0].name), "OverrideJBIG");
  EXPECT_EQ(with_custom.get()[0].scheme, COMPRESSION_JBIG);
  EXPECT_EQ(with_custom.get()[1].scheme, kCustomSchemeB);
  EXPECT_EQ(with_custom.get()[1].init, nullptr);
  EXPECT_EQ(with_custom.get()[2].scheme, kCustomSchemeA);

  size_t total = 0;
  while (with_custom.get()[total].name != nullptr) {
    ++total;
  }
  EXPECT_EQ(with_custom.get()[total].scheme, 0);
  EXPECT_EQ(with_custom.get()[total].init, nullptr);
}

TEST(TifCompressTest, AllocLimitsAndTruncatedStreamsFailWithoutLeaking) {
  DiagnosticRecorder diagnostics;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  diagnostics.Install(opts.get());
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

  // Truncated stream with unknown compression scheme fails cleanly during read.
  std::vector<uint8_t> bytes = BuildMinimalStrippedTiff(
      /*width=*/4U, /*height=*/4U, /*bits_per_sample=*/8,
      /*rows_per_strip=*/4U, kUnknownScheme, /*big_endian=*/false,
      /*big_tiff=*/false);
  bytes.resize(bytes.size() - 2);
  MemoryTiffStream truncated_stream(std::move(bytes));
  UniqueTiff truncated_tif = truncated_stream.Open("r", opts.get());
  if (truncated_tif != nullptr) {
    std::array<uint8_t, 4> row{};
    EXPECT_EQ(TIFFReadScanline(truncated_tif.get(), row.data(), 0, 0), -1);
  }

  // Tight cumulated allocation budget fails buffer setup cleanly before decode.
  MemoryTiffStream valid_stream(BuildMinimalStrippedTiff(
      /*width=*/4U, /*height=*/4U, /*bits_per_sample=*/8,
      /*rows_per_strip=*/4U, kUnknownScheme, /*big_endian=*/false,
      /*big_tiff=*/false));
  UniqueTiff tif = valid_stream.Open("r", opts.get());
  ASSERT_NE(tif, nullptr);
  {
    ScopedCumulatedMemAllocRemaining tight_budget(tif.get(), 64);
    diagnostics.Clear();
    std::array<uint8_t, 4> row{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_GT(diagnostics.error_count(), 0U);
  }

  // Switching to LZW under a tight single-alloc limit causes TIFFInitLZW to
  // fail allocation cleanly and TIFFSetCompressionScheme to propagate 0.
  MemoryTiffStream write_stream;
  UniqueTiff writer = write_stream.Open("w", opts.get());
  ASSERT_NE(writer, nullptr);
  EXPECT_EQ(TIFFSetCompressionScheme(writer.get(), kUnknownScheme), 1);
  {
    ScopedSingleMemAllocLimit single_limit(writer.get(), 16);
    diagnostics.Clear();
    EXPECT_EQ(TIFFSetCompressionScheme(writer.get(), COMPRESSION_LZW), 0);
    EXPECT_GT(diagnostics.error_count(), 0U);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                           static_cast<uint16_t>(COMPRESSION_LZW)),
              0);
  }
  // Once limit is restored, switching to LZW succeeds.
  EXPECT_EQ(TIFFSetCompressionScheme(writer.get(), COMPRESSION_LZW), 1);
  ConfigureStrippedWriteTags(writer.get(), /*width=*/4U, /*height=*/1U,
                             /*bits_per_sample=*/8,
                             /*samples_per_pixel=*/1, /*rows_per_strip=*/1U,
                             PLANARCONFIG_CONTIG, COMPRESSION_LZW);
  std::array<uint8_t, 4> row = {1, 2, 3, 4};
  EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), 1);
}

}  // namespace
}  // namespace libtiff
