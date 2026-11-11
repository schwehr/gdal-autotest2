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

// Unit tests for libtiff/tif_codec.c (builtin codec table,
// TIFFIsCODECConfigured, and NotConfigured / _notConfigured fallbacks).
//
// google3 caveats:
//   - LERC_SUPPORT is defined only in internal tif_config.h (not public
//     tiffconf.h), so tests query TIFFIsCODECConfigured(COMPRESSION_LERC) at
//     runtime rather than guarding with #ifdef LERC_SUPPORT.
//   - Google patches register COMPRESSION_JXL and COMPRESSION_JXL_DNG_1_7 in
//     _TIFFBuiltinCODECS.
//   - COMPRESSION_OJPEG and COMPRESSION_JBIG are built into _TIFFBuiltinCODECS
//     with NotConfigured because OJPEG_SUPPORT and JBIG_SUPPORT are disabled.

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

using ::testing::ElementsAre;

struct BuiltinCodecExpectation {
  uint16_t scheme;
  std::string_view name;
  int expected_configured;
};

constexpr auto kBuiltinCodecExpectations = std::to_array<
    BuiltinCodecExpectation>({
    {.scheme = COMPRESSION_NONE, .name = "None", .expected_configured = 1},
    {.scheme = COMPRESSION_LZW, .name = "LZW", .expected_configured = 1},
    {.scheme = COMPRESSION_PACKBITS,
     .name = "PackBits",
     .expected_configured = 1},
    {.scheme = COMPRESSION_THUNDERSCAN,
     .name = "ThunderScan",
     .expected_configured = 1},
    {.scheme = COMPRESSION_NEXT, .name = "NeXT", .expected_configured = 1},
    {.scheme = COMPRESSION_JPEG, .name = "JPEG", .expected_configured = 1},
    {.scheme = COMPRESSION_JXL, .name = "JXL", .expected_configured = 1},
    {.scheme = COMPRESSION_JXL_DNG_1_7,
     .name = "JXL_DNG_1_7",
     .expected_configured = 1},
    {.scheme = COMPRESSION_OJPEG,
     .name = "Old-style JPEG",
     .expected_configured = 0},
    {.scheme = COMPRESSION_CCITTRLE,
     .name = "CCITT RLE",
     .expected_configured = 1},
    {.scheme = COMPRESSION_CCITTRLEW,
     .name = "CCITT RLE/W",
     .expected_configured = 1},
    {.scheme = COMPRESSION_CCITTFAX3,
     .name = "CCITT Group 3",
     .expected_configured = 1},
    {.scheme = COMPRESSION_CCITTFAX4,
     .name = "CCITT Group 4",
     .expected_configured = 1},
    {.scheme = COMPRESSION_JBIG, .name = "ISO JBIG", .expected_configured = 0},
    {.scheme = COMPRESSION_DEFLATE,
     .name = "Deflate",
     .expected_configured = 1},
    {.scheme = COMPRESSION_ADOBE_DEFLATE,
     .name = "AdobeDeflate",
     .expected_configured = 1},
    {.scheme = COMPRESSION_PIXARLOG,
     .name = "PixarLog",
     .expected_configured = 1},
    {.scheme = COMPRESSION_SGILOG, .name = "SGILog", .expected_configured = 1},
    {.scheme = COMPRESSION_SGILOG24,
     .name = "SGILog24",
     .expected_configured = 1},
    {.scheme = COMPRESSION_LZMA, .name = "LZMA", .expected_configured = 1},
    {.scheme = COMPRESSION_ZSTD, .name = "ZSTD", .expected_configured = 1},
    {.scheme = COMPRESSION_WEBP, .name = "WEBP", .expected_configured = 1},
    {.scheme = COMPRESSION_LERC, .name = "LERC", .expected_configured = 1},
});

int DummyCodecInit(TIFF* /*tif*/, int /*scheme*/) { return 1; }

std::vector<uint8_t> BuildMinimalUnconfiguredStrippedTiff(uint16_t compression,
                                                          bool big_endian,
                                                          bool big_tiff) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const std::array<uint8_t, 4> dummy_strip = {0x10, 0x20, 0x30, 0x40};
  const uint64_t strip_offset = builder.AppendBlob(dummy_strip);

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, 4),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      big_tiff ? builder.MakeLong8(TIFFTAG_STRIPOFFSETS, strip_offset)
               : builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                                  static_cast<uint32_t>(strip_offset)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 4),
      big_tiff ? builder.MakeLong8(TIFFTAG_STRIPBYTECOUNTS, dummy_strip.size())
               : builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                                  static_cast<uint32_t>(dummy_strip.size())),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
  };
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

std::vector<uint8_t> BuildMinimalUnconfiguredTiledTiff(uint16_t compression,
                                                       bool big_endian,
                                                       bool big_tiff) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const std::array<uint8_t, 4> dummy_tile = {0xAA, 0xBB, 0xCC, 0xDD};
  const uint64_t tile_offset = builder.AppendBlob(dummy_tile);

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
      builder.MakeLong(TIFFTAG_TILEWIDTH, 16),
      builder.MakeLong(TIFFTAG_TILELENGTH, 16),
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

bool HasErrorContaining(const DiagnosticRecorder& recorder,
                        std::string_view substr) {
  const std::vector<DiagnosticRecord> errors = recorder.errors();
  return std::any_of(errors.begin(), errors.end(),
                     [substr](const DiagnosticRecord& rec) {
                       return rec.message.find(substr) != std::string::npos;
                     });
}

TEST(TifCodecTest, IsCodecConfiguredMatchesAllBuiltinSchemes) {
  for (const auto& entry : kBuiltinCodecExpectations) {
    SCOPED_TRACE(entry.name);
    const TIFFCodec* codec = TIFFFindCODEC(entry.scheme);
    ASSERT_NE(codec, nullptr);
    EXPECT_EQ(std::string_view(codec->name), entry.name);
    EXPECT_EQ(codec->scheme, entry.scheme);
    EXPECT_NE(codec->init, nullptr);
    EXPECT_EQ(TIFFIsCODECConfigured(entry.scheme), entry.expected_configured);
  }

  // TIFF alias constants map to the same builtin codecs.
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_CCITT_T4), 1);
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_CCITT_T6), 1);
}

TEST(TifCodecTest, BuiltinTableMatchesExpectationsAndTerminatesCleanly) {
  size_t count = 0;
  std::set<uint16_t> seen_schemes;
  for (const TIFFCodec* c = _TIFFBuiltinCODECS; c->name != nullptr; ++c) {
    ASSERT_LT(count, kBuiltinCodecExpectations.size());
    EXPECT_EQ(c->scheme, kBuiltinCodecExpectations[count].scheme);
    EXPECT_EQ(std::string_view(c->name), kBuiltinCodecExpectations[count].name);
    EXPECT_NE(c->init, nullptr);
    EXPECT_TRUE(seen_schemes.insert(c->scheme).second);
    ++count;
  }
  EXPECT_EQ(count, kBuiltinCodecExpectations.size());
  EXPECT_EQ(_TIFFBuiltinCODECS[count].name, nullptr);
  EXPECT_EQ(_TIFFBuiltinCODECS[count].scheme, 0);
  EXPECT_EQ(_TIFFBuiltinCODECS[count].init, nullptr);
}

TEST(TifCodecTest, IsCodecConfiguredReturnsZeroForUnknownSchemes) {
  constexpr auto kUnknownSchemes = std::to_array<uint16_t>({
      0,
      COMPRESSION_T85,
      COMPRESSION_T43,
      COMPRESSION_IT8CTPAD,
      COMPRESSION_IT8LW,
      COMPRESSION_IT8MP,
      COMPRESSION_IT8BL,
      COMPRESSION_PIXARFILM,
      COMPRESSION_DCS,
      COMPRESSION_JP2000,
      65534,
      65535,
  });

  for (uint16_t scheme : kUnknownSchemes) {
    SCOPED_TRACE(scheme);
    EXPECT_EQ(TIFFFindCODEC(scheme), nullptr);
    EXPECT_EQ(TIFFIsCODECConfigured(scheme), 0);
  }
}

TEST(TifCodecTest, GetConfiguredCodecsMatchesExpectedBuiltinSet) {
  UniqueTiffBuffer<TIFFCodec> configured(TIFFGetConfiguredCODECs());
  ASSERT_NE(configured, nullptr);

  std::set<uint16_t> actual_schemes;
  for (const TIFFCodec* c = configured.get(); c->name != nullptr; ++c) {
    EXPECT_NE(c->init, nullptr);
    EXPECT_EQ(TIFFIsCODECConfigured(c->scheme), 1);
    EXPECT_TRUE(actual_schemes.insert(c->scheme).second);
  }

  std::set<uint16_t> expected_schemes;
  for (const auto& entry : kBuiltinCodecExpectations) {
    if (entry.expected_configured == 1) {
      expected_schemes.insert(entry.scheme);
    }
  }
  EXPECT_EQ(actual_schemes, expected_schemes);
  EXPECT_EQ(actual_schemes.count(COMPRESSION_OJPEG), 0U);
  EXPECT_EQ(actual_schemes.count(COMPRESSION_JBIG), 0U);
}

TEST(TifCodecTest, CustomCodecRegistrationAffectsIsCodecConfigured) {
  constexpr uint16_t kCustomValidScheme = 65532;
  constexpr uint16_t kCustomNullInitScheme = 65533;

  EXPECT_EQ(TIFFIsCODECConfigured(kCustomValidScheme), 0);
  EXPECT_EQ(TIFFIsCODECConfigured(kCustomNullInitScheme), 0);

  {
    ScopedRegisteredCodec valid_reg(kCustomValidScheme, "TestCustomCodec",
                                    &DummyCodecInit);
    ASSERT_NE(valid_reg.get(), nullptr);
    EXPECT_NE(TIFFFindCODEC(kCustomValidScheme), nullptr);
    EXPECT_EQ(TIFFIsCODECConfigured(kCustomValidScheme), 1);

    // Registering a codec with a NULL init pointer hits codec->init == NULL.
    ScopedRegisteredCodec null_reg(kCustomNullInitScheme, "TestNullInitCodec",
                                   nullptr);
    ASSERT_NE(null_reg.get(), nullptr);
    const TIFFCodec* found = TIFFFindCODEC(kCustomNullInitScheme);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->init, nullptr);
    EXPECT_EQ(TIFFIsCODECConfigured(kCustomNullInitScheme), 0);
  }

  EXPECT_EQ(TIFFIsCODECConfigured(kCustomValidScheme), 0);
  EXPECT_EQ(TIFFIsCODECConfigured(kCustomNullInitScheme), 0);
}

TEST(TifCodecTest, RegisteredCodecCanTemporarilyOverrideBuiltinScheme) {
  ASSERT_EQ(TIFFIsCODECConfigured(COMPRESSION_JBIG), 0);
  {
    ScopedRegisteredCodec override_jbig(COMPRESSION_JBIG, "MockJBIG",
                                        &DummyCodecInit);
    ASSERT_NE(override_jbig.get(), nullptr);
    EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_JBIG), 1);
  }
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_JBIG), 0);

  ASSERT_EQ(TIFFIsCODECConfigured(COMPRESSION_NONE), 1);
  {
    ScopedRegisteredCodec disable_none(COMPRESSION_NONE, "DisabledNone",
                                       nullptr);
    ASSERT_NE(disable_none.get(), nullptr);
    EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_NONE), 0);
  }
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_NONE), 1);
}

TEST(TifCodecTest, NotConfiguredOnEncodeFailsWithDiagnosticError) {
  struct EncodeCase {
    uint16_t scheme;
    std::string_view expected_name;
    const char* open_mode;
  };
  constexpr auto kCases = std::to_array<EncodeCase>({
      {.scheme = COMPRESSION_JBIG,
       .expected_name = "ISO JBIG",
       .open_mode = "w"},
      {.scheme = COMPRESSION_JBIG,
       .expected_name = "ISO JBIG",
       .open_mode = "wb"},
      {.scheme = COMPRESSION_JBIG,
       .expected_name = "ISO JBIG",
       .open_mode = "w8"},
      {.scheme = COMPRESSION_OJPEG,
       .expected_name = "Old-style JPEG",
       .open_mode = "w"},
      {.scheme = COMPRESSION_OJPEG,
       .expected_name = "Old-style JPEG",
       .open_mode = "wb"},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.expected_name);
    SCOPED_TRACE(tc.open_mode);

    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open(tc.open_mode);
    ASSERT_NE(tif, nullptr);

    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 4U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 4U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 4U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, tc.scheme), 1);

    uint16_t actual_scheme = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_COMPRESSION, &actual_scheme), 1);
    EXPECT_EQ(actual_scheme, tc.scheme);
    EXPECT_EQ(tif->tif_decodestatus, FALSE);
    EXPECT_EQ(tif->tif_encodestatus, FALSE);

    const std::string expected_msg = std::string(tc.expected_name) +
                                     " compression support is not configured";

    std::vector<uint8_t> row(4, 0x5A);
    EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_TRUE(HasErrorContaining(stream.diagnostics(), expected_msg));

    stream.diagnostics().Clear();
    std::vector<uint8_t> strip(16, 0x5A);
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, strip.data(),
                                    static_cast<tmsize_t>(strip.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasErrorContaining(stream.diagnostics(), expected_msg));
  }
}

TEST(TifCodecTest, NotConfiguredOnTiledEncodeFailsWithDiagnosticError) {
  for (uint16_t scheme : {COMPRESSION_JBIG, COMPRESSION_OJPEG}) {
    SCOPED_TRACE(scheme);
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);

    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 16U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 16U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, 16U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, 16U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, scheme), 1);

    std::vector<uint8_t> tile(256, 0x33);
    EXPECT_EQ(TIFFWriteEncodedTile(tif.get(), 0, tile.data(),
                                   static_cast<tmsize_t>(tile.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasErrorContaining(stream.diagnostics(),
                                   "compression support is not configured"));
  }
}

TEST(TifCodecTest, NotConfiguredOnReadTriggersFixupTagsAndSetupDecode) {
  struct ReadCase {
    uint16_t scheme;
    std::string_view expected_name;
    bool big_endian;
    bool big_tiff;
  };
  constexpr auto kCases = std::to_array<ReadCase>({
      {.scheme = COMPRESSION_JBIG,
       .expected_name = "ISO JBIG",
       .big_endian = false,
       .big_tiff = false},
      {.scheme = COMPRESSION_JBIG,
       .expected_name = "ISO JBIG",
       .big_endian = true,
       .big_tiff = false},
      {.scheme = COMPRESSION_JBIG,
       .expected_name = "ISO JBIG",
       .big_endian = false,
       .big_tiff = true},
      {.scheme = COMPRESSION_OJPEG,
       .expected_name = "Old-style JPEG",
       .big_endian = false,
       .big_tiff = false},
      {.scheme = COMPRESSION_OJPEG,
       .expected_name = "Old-style JPEG",
       .big_endian = true,
       .big_tiff = true},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.expected_name);
    SCOPED_TRACE(tc.big_endian ? "BE" : "LE");
    SCOPED_TRACE(tc.big_tiff ? "BigTIFF" : "ClassicTIFF");

    MemoryTiffStream stream(BuildMinimalUnconfiguredStrippedTiff(
        tc.scheme, tc.big_endian, tc.big_tiff));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);

    const std::string expected_msg = std::string(tc.expected_name) +
                                     " compression support is not configured";
    // TIFFReadDirectory invokes (*tif->tif_fixuptags)(tif) -> _notConfigured.
    EXPECT_TRUE(HasErrorContaining(stream.diagnostics(), expected_msg));

    uint16_t actual_scheme = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_COMPRESSION, &actual_scheme), 1);
    EXPECT_EQ(actual_scheme, tc.scheme);

    // TIFFReadScanline invokes (*tif->tif_setupdecode)(tif) -> _notConfigured.
    stream.diagnostics().Clear();
    std::vector<uint8_t> row(4, 0xFF);
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_TRUE(HasErrorContaining(stream.diagnostics(), expected_msg));

    // TIFFReadEncodedStrip also invokes _notConfigured when setupdecode fails.
    stream.diagnostics().Clear();
    std::vector<uint8_t> strip(16, 0xFF);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, strip.data(),
                                   static_cast<tmsize_t>(strip.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasErrorContaining(stream.diagnostics(), expected_msg));
  }
}

TEST(TifCodecTest, NotConfiguredOnTiledReadFailsWithDiagnosticError) {
  for (uint16_t scheme : {COMPRESSION_JBIG, COMPRESSION_OJPEG}) {
    SCOPED_TRACE(scheme);
    MemoryTiffStream stream(
        BuildMinimalUnconfiguredTiledTiff(scheme, false, false));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    EXPECT_TRUE(HasErrorContaining(stream.diagnostics(),
                                   "compression support is not configured"));

    stream.diagnostics().Clear();
    std::vector<uint8_t> tile(256, 0xEE);
    EXPECT_EQ(TIFFReadEncodedTile(tif.get(), 0, tile.data(),
                                  static_cast<tmsize_t>(tile.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasErrorContaining(stream.diagnostics(),
                                   "compression support is not configured"));
  }
}

TEST(TifCodecTest, NotConfiguredFormatsNumericSchemeWhenCodecIsUnknown) {
  MemoryTiffStream stream(
      BuildMinimalUnconfiguredStrippedTiff(COMPRESSION_JBIG, false, false));
  UniqueTiff tif = stream.Open("r");
  ASSERT_NE(tif, nullptr);
  EXPECT_TRUE(HasErrorContaining(
      stream.diagnostics(), "ISO JBIG compression support is not configured"));

  // Exercise the `c ? c->name : compression_code` fallback in _notConfigured
  // while tif_setupdecode remains _notConfigured.
  stream.diagnostics().Clear();
  tif->tif_dir.td_compression = 65534;
  std::vector<uint8_t> row(4, 0x11);
  EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), -1);
  EXPECT_TRUE(HasErrorContaining(
      stream.diagnostics(), "65534 compression support is not configured"));
  tif->tif_dir.td_compression = COMPRESSION_JBIG;
}

TEST(TifCodecTest, SwitchingFromUnconfiguredToNoneRestoresWorkingEncode) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 4U), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 2U), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
            1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 2U), 1);

  // Setting an unconfigured scheme before writing installs NotConfigured hooks.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_OJPEG), 1);
  EXPECT_EQ(tif->tif_decodestatus, FALSE);
  EXPECT_EQ(tif->tif_encodestatus, FALSE);

  // Switching back to COMPRESSION_NONE before TIFFWriteScanline sets
  // TIFF_BEENWRITING replaces the NotConfigured hooks with DumpMode.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
  EXPECT_EQ(tif->tif_decodestatus, TRUE);
  EXPECT_EQ(tif->tif_encodestatus, TRUE);
  std::vector<uint8_t> row0 = {0x42, 0x43, 0x44, 0x45};
  std::vector<uint8_t> row1 = {0x10, 0x20, 0x30, 0x40};
  EXPECT_EQ(TIFFWriteScanline(tif.get(), row0.data(), 0, 0), 1);
  EXPECT_EQ(TIFFWriteScanline(tif.get(), row1.data(), 1, 0), 1);
  tif.reset();

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> decoded(4, 0);
  EXPECT_EQ(TIFFReadScanline(reader.get(), decoded.data(), 0, 0), 1);
  EXPECT_THAT(decoded, ElementsAre(0x42, 0x43, 0x44, 0x45));
  EXPECT_EQ(TIFFReadScanline(reader.get(), decoded.data(), 1, 0), 1);
  EXPECT_THAT(decoded, ElementsAre(0x10, 0x20, 0x30, 0x40));
}

TEST(TifCodecTest, NotConfiguredWithCumulatedAllocLimitFailsCleanly) {
  DiagnosticRecorder diagnostics;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  diagnostics.Install(opts.get());
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

  MemoryTiffStream stream(
      BuildMinimalUnconfiguredStrippedTiff(COMPRESSION_JBIG, false, false));
  UniqueTiff tif = stream.Open("r", opts.get());
  ASSERT_NE(tif, nullptr);
  EXPECT_TRUE(HasErrorContaining(
      diagnostics, "ISO JBIG compression support is not configured"));

  // A 64-byte budget triggers OOM in TIFFReadBufferSetup (1024-byte raw buffer)
  // before reaching _notConfigured, without leaking or crashing.
  {
    ScopedCumulatedMemAllocRemaining tight_budget(tif.get(), 64);
    diagnostics.Clear();
    std::vector<uint8_t> row(4, 0);
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_GT(diagnostics.error_count(), 0U);
  }

  // With enough budget for the raw read buffer, setupdecode reaches
  // _notConfigured and reports the unconfigured codec error.
  {
    ScopedCumulatedMemAllocRemaining adequate_budget(tif.get(), 4096);
    diagnostics.Clear();
    std::vector<uint8_t> row(4, 0);
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_TRUE(HasErrorContaining(
        diagnostics, "ISO JBIG compression support is not configured"));
  }
}

}  // namespace
}  // namespace libtiff
