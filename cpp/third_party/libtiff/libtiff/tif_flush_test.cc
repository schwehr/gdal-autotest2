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

// Clean-room unit tests for libtiff TIFFFlush, TIFFFlushData, and
// TIFFForceStrileArrayWriting (libtiff/tif_flush.c).
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_flush.c

#include <fcntl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/temp_file.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

bool ContainsSubstring(const std::vector<DiagnosticRecord>& records,
                       std::string_view needle) {
  for (const DiagnosticRecord& rec : records) {
    if (rec.message.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void ConfigureStripedGray8(TIFF* tif, uint32_t width, uint32_t length,
                           uint32_t rows_per_strip, uint16_t compression) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, length), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
  ASSERT_EQ(
      TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, uint16_t{PHOTOMETRIC_MINISBLACK}),
      1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, compression), 1);
  ASSERT_EQ(
      TIFFSetField(tif, TIFFTAG_PLANARCONFIG, uint16_t{PLANARCONFIG_CONTIG}),
      1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
}

void ConfigureTiledGray8(TIFF* tif, uint32_t width, uint32_t length,
                         uint32_t tile_width, uint32_t tile_length,
                         uint16_t compression) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, length), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILEWIDTH, tile_width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILELENGTH, tile_length), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
  ASSERT_EQ(
      TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, uint16_t{PHOTOMETRIC_MINISBLACK}),
      1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, compression), 1);
  ASSERT_EQ(
      TIFFSetField(tif, TIFFTAG_PLANARCONFIG, uint16_t{PLANARCONFIG_CONTIG}),
      1);
}

void BuildSimpleGrayTiff(MemoryTiffStream* stream, const char* mode,
                         uint32_t width, uint32_t length,
                         uint32_t rows_per_strip, uint16_t compression,
                         uint8_t base_pixel) {
  UniqueTiff writer = stream->Open(mode);
  ASSERT_NE(writer, nullptr);
  ConfigureStripedGray8(writer.get(), width, length, rows_per_strip,
                        compression);
  std::vector<uint8_t> row(width);
  for (uint32_t y = 0; y < length; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      row[x] = static_cast<uint8_t>((base_pixel + y * width + x) & 0xffU);
    }
    std::vector<uint8_t> mutable_row = row;
    ASSERT_EQ(TIFFWriteScanline(writer.get(), mutable_row.data(), y, 0), 1);
  }
  ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
}

// Custom codec hooks used to test TIFF_POSTENCODE failure handling cleanly.
int NoOpBool(TIFF* /*tif*/) { return 1; }
int NoOpPreEncode(TIFF* /*tif*/, uint16_t /*sample*/) { return 1; }
int NoOpEncodeRow(TIFF* /*tif*/, uint8_t* /*buf*/, tmsize_t /*cc*/,
                  uint16_t /*sample*/) {
  return 1;
}
int FailingPostEncode(TIFF* tif) {
  TIFFErrorExtR(tif, "FailingCodec", "Injected postencode failure");
  return 0;
}
int InitFailingPostEncodeCodec(TIFF* tif, int /*scheme*/) {
  tif->tif_setupencode = &NoOpBool;
  tif->tif_preencode = &NoOpPreEncode;
  tif->tif_postencode = &FailingPostEncode;
  tif->tif_encoderow = &NoOpEncodeRow;
  return 1;
}

TEST(TifFlushTest, FlushAndFlushDataAreNoOpsOnReadOnlyHandle) {
  MemoryTiffStream stream;
  BuildSimpleGrayTiff(&stream, "w", 4, 4, 4, COMPRESSION_NONE, 10);

  const size_t initial_size = stream.data().size();
  const size_t writes_before = stream.write_calls();

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(TIFFGetMode(reader.get()), O_RDONLY);
  EXPECT_EQ(TIFFFlush(reader.get()), 1);
  EXPECT_EQ(TIFFFlushData(reader.get()), 1);
  EXPECT_EQ(stream.write_calls(), writes_before);
  EXPECT_EQ(stream.data().size(), initial_size);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifFlushTest, FlushOnCleanWritableHandleSucceedsWithoutWritingIfd) {
  MemoryTiffStream stream;
  UniqueTiff writer = stream.Open("w");
  ASSERT_NE(writer, nullptr);
  const size_t writes_after_header = stream.write_calls();

  // Newly opened writable TIFF has clean directory and no buffered data.
  EXPECT_EQ(TIFFFlush(writer.get()), 1);
  EXPECT_EQ(TIFFFlushData(writer.get()), 1);
  EXPECT_EQ(stream.write_calls(), writes_after_header);

  // After TIFFWriteDirectory, the fresh empty directory is again clean.
  ConfigureStripedGray8(writer.get(), 4, 2, 2, COMPRESSION_NONE);
  std::array<uint8_t, 4> row = {1, 2, 3, 4};
  ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), 1);
  ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), 1, 0), 1);
  ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);

  const size_t writes_after_dir = stream.write_calls();
  EXPECT_EQ(TIFFFlush(writer.get()), 1);
  EXPECT_EQ(stream.write_calls(), writes_after_dir);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifFlushTest, FlushMakesBufferedScanlinesAndDirectoryReadableAcrossModes) {
  constexpr std::array<const char*, 4> kWriteModes = {"wl", "wb", "w8l", "w8b"};

  for (const char* mode : kWriteModes) {
    SCOPED_TRACE(mode);
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open(mode);
    ASSERT_NE(writer, nullptr);

    constexpr uint32_t kWidth = 6;
    constexpr uint32_t kLength = 4;
    ConfigureStripedGray8(writer.get(), kWidth, kLength, kLength,
                          COMPRESSION_NONE);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEDESCRIPTION, "flushed"),
              1);

    std::vector<std::array<uint8_t, kWidth>> expected_rows(kLength);
    for (uint32_t y = 0; y < kLength; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
        expected_rows[y][x] = static_cast<uint8_t>(0x30U + y * 10U + x);
      }
      std::array<uint8_t, kWidth> mutable_row = expected_rows[y];
      ASSERT_EQ(TIFFWriteScanline(writer.get(), mutable_row.data(), y, 0), 1);
    }

    // Small uncompressed scanlines remain buffered until TIFFFlush.
    EXPECT_GT(writer->tif_rawcc, 0);
    EXPECT_EQ(TIFFFlush(writer.get()), 1);
    EXPECT_EQ(writer->tif_rawcc, 0);
    EXPECT_EQ(writer->tif_flags & (TIFF_DIRTYDIRECT | TIFF_DIRTYSTRIP), 0U);

    // Verify a second reader on the flushed byte stream sees all tags & pixels
    // before the writer handle is closed.
    MemoryTiffStream read_stream(stream.data());
    UniqueTiff reader = read_stream.Open("r");
    ASSERT_NE(reader, nullptr);

    uint32_t w = 0;
    uint32_t h = 0;
    uint16_t bps = 0;
    uint16_t spp = 0;
    const char* desc = nullptr;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGEWIDTH, &w), 1);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGELENGTH, &h), 1);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_BITSPERSAMPLE, &bps), 1);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_SAMPLESPERPIXEL, &spp), 1);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGEDESCRIPTION, &desc), 1);
    EXPECT_EQ(w, kWidth);
    EXPECT_EQ(h, kLength);
    EXPECT_EQ(bps, 8U);
    EXPECT_EQ(spp, 1U);
    ASSERT_NE(desc, nullptr);
    EXPECT_STREQ(desc, "flushed");

    std::array<uint8_t, kWidth> actual_row{};
    for (uint32_t y = 0; y < kLength; ++y) {
      ASSERT_EQ(TIFFReadScanline(reader.get(), actual_row.data(), y, 0), 1);
      EXPECT_EQ(actual_row, expected_rows[y]);
    }
    EXPECT_EQ(read_stream.diagnostics().error_count(), 0U);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifFlushTest, FlushDataWithAndWithoutPostEncodeFlag) {
  MemoryTiffStream stream;
  UniqueTiff writer = stream.Open("w");
  ASSERT_NE(writer, nullptr);

  constexpr uint32_t kWidth = 8;
  constexpr uint32_t kLength = 2;
  ConfigureStripedGray8(writer.get(), kWidth, kLength, kLength,
                        COMPRESSION_NONE);

  // Writing scanline 0 in a new strip sets TIFF_BEENWRITING and
  // TIFF_POSTENCODE.
  std::array<uint8_t, kWidth> row0 = {1, 2, 3, 4, 5, 6, 7, 8};
  std::array<uint8_t, kWidth> mutable_row0 = row0;
  ASSERT_EQ(TIFFWriteScanline(writer.get(), mutable_row0.data(), 0, 0), 1);
  EXPECT_NE(writer->tif_flags & TIFF_BEENWRITING, 0U);
  EXPECT_NE(writer->tif_flags & TIFF_POSTENCODE, 0U);
  EXPECT_EQ(TIFFFlushData(writer.get()), 1);
  EXPECT_EQ(writer->tif_flags & TIFF_POSTENCODE, 0U);
  EXPECT_EQ(writer->tif_rawcc, 0);

  // Writing scanline 1 in the SAME strip leaves TIFF_POSTENCODE clear so the
  // second TIFFFlushData exercises the non-postencode path directly.
  std::array<uint8_t, kWidth> row1 = {9, 10, 11, 12, 13, 14, 15, 16};
  std::array<uint8_t, kWidth> mutable_row1 = row1;
  ASSERT_EQ(TIFFWriteScanline(writer.get(), mutable_row1.data(), 1, 0), 1);
  EXPECT_EQ(writer->tif_flags & TIFF_POSTENCODE, 0U);
  EXPECT_GT(writer->tif_rawcc, 0);
  EXPECT_EQ(TIFFFlushData(writer.get()), 1);
  EXPECT_EQ(writer->tif_rawcc, 0);

  writer.reset();

  MemoryTiffStream read_stream(stream.data());
  UniqueTiff reader = read_stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::array<uint8_t, kWidth> actual{};
  ASSERT_EQ(TIFFReadScanline(reader.get(), actual.data(), 0, 0), 1);
  EXPECT_EQ(actual, row0);
  ASSERT_EQ(TIFFReadScanline(reader.get(), actual.data(), 1, 0), 1);
  EXPECT_EQ(actual, row1);
}

TEST(TifFlushTest, FlushDataInvokesDeflatePostEncodeAndPropagatesFailure) {
  if (!TIFFIsCODECConfigured(COMPRESSION_ADOBE_DEFLATE)) {
    GTEST_SKIP() << "COMPRESSION_ADOBE_DEFLATE not configured";
  }

  // Success path: Deflate buffers encoded stream until postencode runs.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    constexpr uint32_t kWidth = 16;
    constexpr uint32_t kLength = 4;
    ConfigureStripedGray8(writer.get(), kWidth, kLength, kLength,
                          COMPRESSION_ADOBE_DEFLATE);

    std::vector<uint8_t> row(kWidth, 0x5AU);
    for (uint32_t y = 0; y < kLength; ++y) {
      std::vector<uint8_t> mutable_row = row;
      ASSERT_EQ(TIFFWriteScanline(writer.get(), mutable_row.data(), y, 0), 1);
    }
    const size_t size_before_flush = stream.data().size();
    EXPECT_NE(writer->tif_flags & TIFF_POSTENCODE, 0U);
    EXPECT_EQ(TIFFFlushData(writer.get()), 1);
    EXPECT_EQ(writer->tif_flags & TIFF_POSTENCODE, 0U);
    EXPECT_GT(stream.data().size(), size_before_flush);
    EXPECT_EQ(TIFFFlush(writer.get()), 1);
    writer.reset();

    MemoryTiffStream read_stream(stream.data());
    UniqueTiff reader = read_stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> decoded(kWidth);
    for (uint32_t y = 0; y < kLength; ++y) {
      ASSERT_EQ(TIFFReadScanline(reader.get(), decoded.data(), y, 0), 1);
      EXPECT_EQ(decoded, row);
    }
  }

  // Failure path: write error during ZIPPostEncode causes TIFFFlushData and
  // TIFFFlush to return 0.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedGray8(writer.get(), 16, 2, 2, COMPRESSION_ADOBE_DEFLATE);
    std::vector<uint8_t> row(16, 0x3CU);
    ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), 1);

    stream.FailWritesAfter(stream.write_calls());
    EXPECT_EQ(TIFFFlush(writer.get()), 0);
    EXPECT_GT(stream.diagnostics().error_count(), 0U);

    stream.ResetFaults();
  }
}

TEST(TifFlushTest, FlushDataAndFlushPropagateCustomPostEncodeFailure) {
  constexpr uint16_t kFakeScheme = 61234;
  ASSERT_EQ(TIFFFindCODEC(kFakeScheme), nullptr);
  ScopedRegisteredCodec codec(kFakeScheme, "FailingPostEncodeCodec",
                              &InitFailingPostEncodeCodec);
  ASSERT_NE(codec.get(), nullptr);

  MemoryTiffStream stream;
  UniqueTiff writer = stream.Open("w");
  ASSERT_NE(writer, nullptr);
  ConfigureStripedGray8(writer.get(), 4, 2, 2, kFakeScheme);

  std::array<uint8_t, 4> row = {1, 2, 3, 4};
  ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), 1);
  ASSERT_NE(writer->tif_flags & TIFF_POSTENCODE, 0U);

  EXPECT_EQ(TIFFFlush(writer.get()), 0);
  EXPECT_EQ(writer->tif_flags & TIFF_POSTENCODE, 0U);
  EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                "Injected postencode failure"));

  // Re-arm TIFF_POSTENCODE via TIFFFlushData directly after resetting strip.
  std::array<uint8_t, 4> row1 = {5, 6, 7, 8};
  writer->tif_flags |= TIFF_POSTENCODE;
  ASSERT_EQ(TIFFWriteScanline(writer.get(), row1.data(), 1, 0), 1);
  EXPECT_EQ(TIFFFlushData(writer.get()), 0);
}

TEST(TifFlushTest, FlushDataAndFlushPropagateFlushData1WriteFailure) {
  MemoryTiffStream stream;
  UniqueTiff writer = stream.Open("w");
  ASSERT_NE(writer, nullptr);
  ConfigureStripedGray8(writer.get(), 8, 2, 2, COMPRESSION_NONE);

  // Flush scanline 0 so TIFF_POSTENCODE is cleared.
  std::array<uint8_t, 8> row0 = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_EQ(TIFFWriteScanline(writer.get(), row0.data(), 0, 0), 1);
  ASSERT_EQ(TIFFFlushData(writer.get()), 1);

  // Buffer scanline 1 in the same strip (TIFF_POSTENCODE stays 0).
  std::array<uint8_t, 8> row1 = {9, 10, 11, 12, 13, 14, 15, 16};
  ASSERT_EQ(TIFFWriteScanline(writer.get(), row1.data(), 1, 0), 1);
  ASSERT_EQ(writer->tif_flags & TIFF_POSTENCODE, 0U);
  ASSERT_GT(writer->tif_rawcc, 0);

  stream.FailWritesAfter(stream.write_calls());
  EXPECT_EQ(TIFFFlushData(writer.get()), 0);
  EXPECT_GT(stream.diagnostics().error_count(), 0U);

  stream.ResetFaults();
}

TEST(TifFlushTest, FlushUpdateModeDirtyStripUsesForceStrileArrayWriting) {
  ASSERT_EQ(TIFFIsCODECConfigured(COMPRESSION_PACKBITS), 1);

  constexpr uint32_t kWidth = 32;
  constexpr uint32_t kLength = 4;
  constexpr uint32_t kRowsPerStrip = 2;
  constexpr uint32_t kStripBytes = kWidth * kRowsPerStrip;

  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedGray8(writer.get(), kWidth, kLength, kRowsPerStrip,
                          COMPRESSION_PACKBITS);
    // Uniform bytes compress very small (~2 bytes per run), so overwriting with
    // incompressible data later forces relocation at EOF and sets
    // TIFF_DIRTYSTRIP without dirtying any non-strip directory field.
    std::vector<uint8_t> strip0(kStripBytes, 0xAAU);
    std::vector<uint8_t> strip1(kStripBytes, 0xBBU);
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip0.data(),
                                    static_cast<tmsize_t>(strip0.size())),
              static_cast<tmsize_t>(kStripBytes));
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, strip1.data(),
                                    static_cast<tmsize_t>(strip1.size())),
              static_cast<tmsize_t>(kStripBytes));
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  UniqueTiff updater = stream.Open("r+");
  ASSERT_NE(updater, nullptr);
  const uint64_t original_dir_offset = TIFFCurrentDirOffset(updater.get());
  ASSERT_NE(original_dir_offset, 0U);

  std::vector<uint8_t> updated_strip0(kStripBytes);
  for (uint32_t i = 0; i < kStripBytes; ++i) {
    updated_strip0[i] = static_cast<uint8_t>((i * 7U + 3U) & 0xffU);
  }
  std::vector<uint8_t> mutable_strip0 = updated_strip0;
  ASSERT_EQ(TIFFWriteEncodedStrip(updater.get(), 0, mutable_strip0.data(),
                                  static_cast<tmsize_t>(mutable_strip0.size())),
            static_cast<tmsize_t>(kStripBytes));

  EXPECT_NE(updater->tif_flags & TIFF_DIRTYSTRIP, 0U);
  EXPECT_EQ(updater->tif_flags & TIFF_DIRTYDIRECT, 0U);
  EXPECT_EQ(TIFFFlush(updater.get()), 1);

  // TIFFForceStrileArrayWriting updated the strile arrays without relocating
  // the IFD itself.
  EXPECT_EQ(TIFFCurrentDirOffset(updater.get()), original_dir_offset);
  EXPECT_EQ(updater->tif_flags & (TIFF_DIRTYSTRIP | TIFF_BEENWRITING), 0U);
  updater.reset();

  MemoryTiffStream read_stream(stream.data());
  UniqueTiff reader = read_stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> read_strip0(kStripBytes);
  std::vector<uint8_t> read_strip1(kStripBytes);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, read_strip0.data(),
                                 static_cast<tmsize_t>(read_strip0.size())),
            static_cast<tmsize_t>(kStripBytes));
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 1, read_strip1.data(),
                                 static_cast<tmsize_t>(read_strip1.size())),
            static_cast<tmsize_t>(kStripBytes));
  EXPECT_EQ(read_strip0, updated_strip0);
  EXPECT_EQ(read_strip1, std::vector<uint8_t>(kStripBytes, 0xBBU));
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifFlushTest, FlushPropagatesDirectoryRewriteAndFallbackFailures) {
  // 1. Direct TIFFRewriteDirectory failure when TIFF_DIRTYDIRECT is set.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedGray8(writer.get(), 4, 2, 2, COMPRESSION_NONE);
    stream.FailWritesAfter(stream.write_calls());
    EXPECT_EQ(TIFFFlush(writer.get()), 0);
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
    stream.ResetFaults();
  }

  // 2. Update mode ("r+") with only TIFF_DIRTYSTRIP:
  // TIFFForceStrileArrayWriting fails on I/O fault and falls through to
  // TIFFRewriteDirectory, which fails.
  {
    ASSERT_EQ(TIFFIsCODECConfigured(COMPRESSION_PACKBITS), 1);
    constexpr uint32_t kStripBytes = 64;
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedGray8(writer.get(), 32, 2, 2, COMPRESSION_PACKBITS);
      std::vector<uint8_t> uniform(kStripBytes, 0x11U);
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, uniform.data(),
                                      static_cast<tmsize_t>(uniform.size())),
                static_cast<tmsize_t>(kStripBytes));
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff updater = stream.Open("r+");
    ASSERT_NE(updater, nullptr);
    std::vector<uint8_t> dense(kStripBytes);
    for (uint32_t i = 0; i < kStripBytes; ++i) {
      dense[i] = static_cast<uint8_t>(i);
    }
    ASSERT_EQ(TIFFWriteEncodedStrip(updater.get(), 0, dense.data(),
                                    static_cast<tmsize_t>(dense.size())),
              static_cast<tmsize_t>(kStripBytes));
    ASSERT_NE(updater->tif_flags & TIFF_DIRTYSTRIP, 0U);
    ASSERT_EQ(updater->tif_flags & TIFF_DIRTYDIRECT, 0U);

    stream.FailWritesAfter(stream.write_calls());
    EXPECT_EQ(TIFFFlush(updater.get()), 0);
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
    stream.ResetFaults();
  }
}

TEST(TifFlushTest, ForceStrileArrayWritingRejectsInvalidStates) {
  MemoryTiffStream base_stream;
  BuildSimpleGrayTiff(&base_stream, "w", 4, 4, 2, COMPRESSION_NONE, 1);

  // 1. Read-only handle.
  {
    MemoryTiffStream ro_stream(base_stream.data());
    UniqueTiff reader = ro_stream.Open("r");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFForceStrileArrayWriting(reader.get()), 0);
    EXPECT_TRUE(ContainsSubstring(ro_stream.diagnostics().errors(),
                                  "File opened in read-only mode"));
  }

  // 2. Directory not yet written (tif_diroff == 0).
  {
    MemoryTiffStream new_stream;
    UniqueTiff writer = new_stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedGray8(writer.get(), 4, 4, 2, COMPRESSION_NONE);
    EXPECT_EQ(TIFFForceStrileArrayWriting(writer.get()), 0);
    EXPECT_TRUE(ContainsSubstring(new_stream.diagnostics().errors(),
                                  "Directory has not yet been written"));
  }

  // 3. Directory has non-strile dirty tags (TIFF_DIRTYDIRECT).
  {
    MemoryTiffStream rw_stream(base_stream.data());
    UniqueTiff updater = rw_stream.Open("r+");
    ASSERT_NE(updater, nullptr);
    ASSERT_EQ(TIFFSetField(updater.get(), TIFFTAG_IMAGEDESCRIPTION, "dirty"),
              1);
    EXPECT_EQ(TIFFForceStrileArrayWriting(updater.get()), 0);
    EXPECT_TRUE(
        ContainsSubstring(rw_stream.diagnostics().errors(),
                          "TIFFRewriteDirectory() should be called instead"));
  }

  // 4. Non-deferred directory with clean strile arrays.
  {
    MemoryTiffStream rw_stream(base_stream.data());
    UniqueTiff updater = rw_stream.Open("r+");
    ASSERT_NE(updater, nullptr);
    EXPECT_EQ(TIFFForceStrileArrayWriting(updater.get()), 0);
    EXPECT_TRUE(ContainsSubstring(rw_stream.diagnostics().errors(),
                                  "TIFFDeferStrileArrayWriting()"));
  }
}

TEST(TifFlushTest, ForceStrileArrayWritingDeferredStripsAndTiles) {
  struct DeferredCase {
    const char* write_mode;
    bool tiled;
  };
  constexpr std::array<DeferredCase, 6> kCases = {{
      {.write_mode = "wl", .tiled = false},
      {.write_mode = "wb", .tiled = false},
      {.write_mode = "w8l", .tiled = false},
      {.write_mode = "wl", .tiled = true},
      {.write_mode = "wb", .tiled = true},
      {.write_mode = "w8b", .tiled = true},
  }};

  for (const DeferredCase& tc : kCases) {
    SCOPED_TRACE(testing::Message()
                 << tc.write_mode << (tc.tiled ? " tiled" : " striped"));
    MemoryTiffStream stream;
    {
      UniqueTiff tif = stream.Open(tc.write_mode);
      ASSERT_NE(tif, nullptr);

      const uint32_t width = tc.tiled ? 32 : 8;
      const uint32_t length = tc.tiled ? 32 : 8;
      const tmsize_t expected_strile_bytes = tc.tiled ? 256 : 16;
      if (tc.tiled) {
        ConfigureTiledGray8(tif.get(), width, length, 16, 16, COMPRESSION_NONE);
      } else {
        ConfigureStripedGray8(tif.get(), width, length, 2, COMPRESSION_NONE);
      }
      ASSERT_EQ(TIFFDeferStrileArrayWriting(tif.get()), 1);
      ASSERT_EQ(TIFFWriteCheck(tif.get(), tc.tiled ? 1 : 0, "TifFlushTest"), 1);
      ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);

      // Write a second directory so we switch back to directory 0 before
      // forcing strile array writing.
      ConfigureStripedGray8(tif.get(), 4, 2, 2, COMPRESSION_NONE);
      std::array<uint8_t, 8> dir1_strip = {90, 91, 92, 93, 94, 95, 96, 97};
      std::array<uint8_t, 8> mutable_dir1 = dir1_strip;
      ASSERT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, mutable_dir1.data(), 8), 8);
      ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);

      // Return to directory 0 and force writing the deferred dummy arrays.
      ASSERT_EQ(TIFFSetDirectory(tif.get(), 0), 1);
      EXPECT_EQ(tif->tif_flags & TIFF_DIRTYSTRIP, 0U);
      EXPECT_EQ(TIFFForceStrileArrayWriting(tif.get()), 1);
      EXPECT_EQ(tif->tif_flags & (TIFF_DIRTYSTRIP | TIFF_BEENWRITING), 0U);

      // Populate all strips or tiles in directory 0 and rewrite strile arrays.
      const uint32_t nstriles = tc.tiled ? TIFFNumberOfTiles(tif.get())
                                         : TIFFNumberOfStrips(tif.get());
      ASSERT_EQ(nstriles, 4U);
      const tmsize_t strile_bytes =
          tc.tiled ? TIFFTileSize(tif.get()) : TIFFStripSize(tif.get());
      ASSERT_EQ(strile_bytes, expected_strile_bytes);

      for (uint32_t i = 0; i < nstriles; ++i) {
        std::vector<uint8_t> payload(static_cast<size_t>(strile_bytes),
                                     static_cast<uint8_t>(0x20U + i));
        if (tc.tiled) {
          ASSERT_EQ(
              TIFFWriteEncodedTile(tif.get(), i, payload.data(), strile_bytes),
              strile_bytes);
        } else {
          ASSERT_EQ(
              TIFFWriteEncodedStrip(tif.get(), i, payload.data(), strile_bytes),
              strile_bytes);
        }
      }
      EXPECT_NE(tif->tif_flags & TIFF_DIRTYSTRIP, 0U);
      EXPECT_EQ(TIFFForceStrileArrayWriting(tif.get()), 1);
      EXPECT_EQ(tif->tif_flags & (TIFF_DIRTYSTRIP | TIFF_BEENWRITING), 0U);
    }

    MemoryTiffStream read_stream(stream.data());
    UniqueTiff reader = read_stream.Open("r");
    ASSERT_NE(reader, nullptr);
    ASSERT_EQ(TIFFNumberOfDirectories(reader.get()), 2U);
    ASSERT_EQ(TIFFSetDirectory(reader.get(), 0), 1);
    const uint32_t nstriles = tc.tiled ? TIFFNumberOfTiles(reader.get())
                                       : TIFFNumberOfStrips(reader.get());
    ASSERT_EQ(nstriles, 4U);
    const tmsize_t expected_strile_bytes = tc.tiled ? 256 : 16;
    std::vector<uint8_t> buf(static_cast<size_t>(expected_strile_bytes));
    for (uint32_t i = 0; i < nstriles; ++i) {
      if (tc.tiled) {
        ASSERT_EQ(TIFFReadEncodedTile(reader.get(), i, buf.data(),
                                      expected_strile_bytes),
                  expected_strile_bytes);
      } else {
        ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), i, buf.data(),
                                       expected_strile_bytes),
                  expected_strile_bytes);
      }
      EXPECT_EQ(buf,
                std::vector<uint8_t>(static_cast<size_t>(expected_strile_bytes),
                                     static_cast<uint8_t>(0x20U + i)));
    }
    ASSERT_EQ(TIFFSetDirectory(reader.get(), 1), 1);
    std::array<uint8_t, 8> dir1_actual{};
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, dir1_actual.data(), 8), 8);
    EXPECT_EQ(dir1_actual,
              (std::array<uint8_t, 8>{90, 91, 92, 93, 94, 95, 96, 97}));
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    EXPECT_EQ(read_stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifFlushTest, ForceStrileArrayWritingSetupStripsOomFailsAndRecovers) {
  // Use enough strips so td_stripoffset_p / td_stripbytecount_p allocations
  // (1024 * 8 = 8192 bytes) exceed our temporary single-allocation limit while
  // IFD metadata allocations (< 4096 bytes) still succeed.
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedGray8(writer.get(), 4, 1024, 1, COMPRESSION_NONE);
    ASSERT_EQ(TIFFDeferStrileArrayWriting(writer.get()), 1);
    ASSERT_EQ(TIFFWriteCheck(writer.get(), 0, "TifFlushTest"), 1);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  UniqueTiff updater = stream.Open("r+h");
  ASSERT_NE(updater, nullptr);
  {
    ScopedSingleMemAllocLimit limit(updater.get(), 4096);
    ASSERT_EQ(TIFFReadDirectory(updater.get()), 1);
    ASSERT_EQ(updater->tif_dir.td_stripoffset_p, nullptr);
    EXPECT_EQ(TIFFForceStrileArrayWriting(updater.get()), 0);
  }
  EXPECT_GT(stream.diagnostics().error_count(), 0U);
  stream.diagnostics().Clear();

  // With the allocation limit lifted, TIFFSetupStrips succeeds and writes out
  // the deferred strile arrays.
  EXPECT_EQ(TIFFForceStrileArrayWriting(updater.get()), 1);
  EXPECT_NE(updater->tif_dir.td_stripoffset_p, nullptr);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifFlushTest, ForceStrileArrayWritingPropagatesRewriteFieldErrors) {
  MemoryTiffStream base_stream;
  {
    UniqueTiff writer = base_stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedGray8(writer.get(), 4, 4, 2, COMPRESSION_NONE);
    ASSERT_EQ(TIFFDeferStrileArrayWriting(writer.get()), 1);
    ASSERT_EQ(TIFFWriteCheck(writer.get(), 0, "TifFlushTest"), 1);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  // 1. First _TIFFRewriteField (STRIPOFFSETS) fails on write.
  {
    MemoryTiffStream stream(base_stream.data());
    UniqueTiff updater = stream.Open("r+");
    ASSERT_NE(updater, nullptr);
    stream.FailWritesAfter(stream.write_calls());
    EXPECT_EQ(TIFFForceStrileArrayWriting(updater.get()), 0);
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
    stream.ResetFaults();
  }

  // 2. First _TIFFRewriteField succeeds (2 writes for deferred 2-strip array),
  //    second _TIFFRewriteField (STRIPBYTECOUNTS) fails.
  {
    MemoryTiffStream stream(base_stream.data());
    UniqueTiff updater = stream.Open("r+");
    ASSERT_NE(updater, nullptr);
    stream.FailWritesAfter(stream.write_calls() + 2);
    EXPECT_EQ(TIFFForceStrileArrayWriting(updater.get()), 0);
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
    stream.ResetFaults();
  }

  // 3. Seek error during _TIFFRewriteField.
  {
    MemoryTiffStream stream(base_stream.data());
    UniqueTiff updater = stream.Open("r+");
    ASSERT_NE(updater, nullptr);
    stream.FailSeeksAfter(stream.seek_calls());
    EXPECT_EQ(TIFFForceStrileArrayWriting(updater.get()), 0);
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
    stream.ResetFaults();
  }
}

TEST(TifFlushTest, FlushPersistsBufferedPixelsAndDirectoryToTempFile) {
  TempFile temp_file("tif_flush_disk_roundtrip.tif");
  DiagnosticRecorder recorder;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  recorder.Install(opts.get());

  UniqueTiff writer(TIFFOpenExt(temp_file.c_str(), "w", opts.get()));
  ASSERT_NE(writer, nullptr);
  constexpr uint32_t kWidth = 4;
  constexpr uint32_t kLength = 3;
  ConfigureStripedGray8(writer.get(), kWidth, kLength, kLength,
                        COMPRESSION_NONE);

  std::array<std::array<uint8_t, kWidth>, kLength> expected = {{
      {11, 12, 13, 14},
      {21, 22, 23, 24},
      {31, 32, 33, 34},
  }};
  for (uint32_t y = 0; y < kLength; ++y) {
    std::array<uint8_t, kWidth> row = expected[y];
    ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), y, 0), 1);
  }
  ASSERT_EQ(TIFFFlush(writer.get()), 1);

  // Open a second handle from disk before writer closes.
  UniqueTiff reader(TIFFOpenExt(temp_file.c_str(), "r", opts.get()));
  ASSERT_NE(reader, nullptr);
  for (uint32_t y = 0; y < kLength; ++y) {
    std::array<uint8_t, kWidth> actual{};
    ASSERT_EQ(TIFFReadScanline(reader.get(), actual.data(), y, 0), 1);
    EXPECT_EQ(actual, expected[y]);
  }
  EXPECT_EQ(recorder.error_count(), 0U);
}

}  // namespace
}  // namespace libtiff
