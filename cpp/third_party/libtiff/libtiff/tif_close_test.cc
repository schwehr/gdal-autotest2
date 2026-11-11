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

// Unit tests for libtiff tif_close.c (TIFFClose, TIFFCleanup, and
// _TIFFCleanupIFDOffsetAndNumberMaps).

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
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
#include "third_party/tiff/google/util/temp_file.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

using ::testing::HasSubstr;

constexpr uint32_t kAnonLongTag = 65000;
constexpr uint32_t kAnonAsciiTag = 65001;
constexpr uint32_t kAnonShortArrayTag = 65002;
constexpr uint32_t kMergedLongTag = 65100;
constexpr uint32_t kMergedAsciiTag = 65101;
constexpr uint32_t kExtenderTag = 65102;

void WriteRgbImage(TIFF* tif, uint32_t width, uint32_t height, uint8_t seed,
                   uint16_t compression = COMPRESSION_NONE) {
  ASSERT_NE(tif, nullptr);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, compression), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, height), 1);

  const size_t row_bytes = static_cast<size_t>(width) * 3;
  for (uint32_t row_idx = 0; row_idx < height; ++row_idx) {
    // Use a fresh mutable buffer because TIFFWriteScanline may mutate in place.
    std::vector<uint8_t> row(row_bytes);
    for (size_t i = 0; i < row_bytes; ++i) {
      row[i] = static_cast<uint8_t>(seed + row_idx * 17 + i * 3);
    }
    EXPECT_EQ(TIFFWriteScanline(tif, row.data(), row_idx, 0), 1);
  }
}

void VerifyRgbImage(TIFF* tif, uint32_t expected_width,
                    uint32_t expected_height, uint8_t seed) {
  ASSERT_NE(tif, nullptr);
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t bps = 0;
  uint16_t spp = 0;
  EXPECT_EQ(TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width), 1);
  EXPECT_EQ(TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height), 1);
  EXPECT_EQ(TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bps), 1);
  EXPECT_EQ(TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp), 1);
  EXPECT_EQ(width, expected_width);
  EXPECT_EQ(height, expected_height);
  EXPECT_EQ(bps, 8);
  EXPECT_EQ(spp, 3);

  const size_t row_bytes = static_cast<size_t>(expected_width) * 3;
  std::vector<uint8_t> actual(row_bytes);
  for (uint32_t row_idx = 0; row_idx < expected_height; ++row_idx) {
    SCOPED_TRACE(row_idx);
    ASSERT_EQ(TIFFReadScanline(tif, actual.data(), row_idx, 0), 1);
    for (size_t i = 0; i < row_bytes; ++i) {
      const uint8_t expected =
          static_cast<uint8_t>(seed + row_idx * 17 + i * 3);
      EXPECT_EQ(actual[i], expected);
    }
  }
}

std::vector<uint8_t> BuildRawGrayscaleTiffWithUnknownTags(bool big_endian,
                                                          bool big_tiff) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const std::array<uint8_t, 4> pixels = {10, 20, 30, 40};
  const uint64_t strip_off = builder.AppendBlob(pixels);
  const std::array<uint16_t, 3> anon_shorts = {11, 22, 33};

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, 2),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, 2),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(strip_off)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 2),
      builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                       static_cast<uint32_t>(pixels.size())),
      builder.MakeAscii(TIFFTAG_ARTIST, "GoogleTest"),
      builder.MakeLong(kAnonLongTag, 0x12345678U),
      builder.MakeAscii(kAnonAsciiTag, "anon_value"),
      builder.MakeShortArray(kAnonShortArrayTag, anon_shorts),
  };
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

std::vector<uint8_t> BuildMultiIfdRawTiff(bool big_endian, bool big_tiff,
                                          int num_ifds) {
  RawTiffBuilder builder(big_endian, big_tiff);
  for (int i = 0; i < num_ifds; ++i) {
    const uint32_t dim = static_cast<uint32_t>(2 + i);
    std::vector<uint8_t> pixels(dim * dim, static_cast<uint8_t>(40 + i * 15));
    const uint64_t strip_off = builder.AppendBlob(pixels);
    std::vector<RawIfdEntry> entries = {
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, dim),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, dim),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                         static_cast<uint32_t>(strip_off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, dim),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(pixels.size())),
    };
    builder.AddIfd(std::move(entries));
  }
  return builder.Take();
}

void CustomTagExtender(TIFF* tif) {
  static const TIFFFieldInfo kExtenderFields[] = {
      {kExtenderTag, 1, 1, TIFF_LONG, FIELD_CUSTOM, 1, 0, "ExtenderLong"},
  };
  EXPECT_EQ(TIFFMergeFieldInfo(tif, kExtenderFields, 1), 0);
}

TEST(TifCloseTest, CloseNullPointerIsSafe) { TIFFClose(nullptr); }

TEST(TifCloseTest, CloseReadOnlyUnmappedDoesNotFlushOrUnmap) {
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    WriteRgbImage(writer.get(), 4, 3, 5);
  }
  EXPECT_TRUE(stream.is_closed());
  EXPECT_EQ(stream.close_calls(), 1);
  EXPECT_EQ(stream.diagnostics().error_count(), 0);

  const size_t writes_before_read = stream.write_calls();
  UniqueTiff reader = stream.Open("rm");
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(TIFFGetMode(reader.get()), O_RDONLY);
  EXPECT_EQ(reader->tif_flags & TIFF_MAPPED, 0U);
  VerifyRgbImage(reader.get(), 4, 3, 5);

  // Reading scanlines in unmapped mode sets TIFF_MYBUFFER for tif_rawdata.
  EXPECT_NE(reader->tif_rawdata, nullptr);
  EXPECT_NE(reader->tif_flags & TIFF_MYBUFFER, 0U);

  reader.reset();
  EXPECT_TRUE(stream.is_closed());
  EXPECT_EQ(stream.close_calls(), 2);
  EXPECT_EQ(stream.write_calls(), writes_before_read);
  EXPECT_EQ(stream.unmap_calls(), 0);
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
}

TEST(TifCloseTest, CloseMappedMemoryStreamUnmapsCleanly) {
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    WriteRgbImage(writer.get(), 4, 4, 19);
  }

  stream.set_enable_mmap(true);
  UniqueTiff reader = stream.Open("rM");
  ASSERT_NE(reader, nullptr);
  EXPECT_NE(reader->tif_flags & TIFF_MAPPED, 0U);
  EXPECT_EQ(stream.map_calls(), 1);
  EXPECT_EQ(stream.unmap_calls(), 0);

  VerifyRgbImage(reader.get(), 4, 4, 19);
  // In mmap mode, rawdata aliases tif_base and clears TIFF_MYBUFFER.
  EXPECT_EQ(reader->tif_flags & TIFF_MYBUFFER, 0U);

  reader.reset();
  EXPECT_EQ(stream.unmap_calls(), 1);
  EXPECT_TRUE(stream.is_closed());
  EXPECT_EQ(stream.close_calls(), 2);
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
}

TEST(TifCloseTest, CloseMappedTempFileUnmapsCleanly) {
  TempFile temp_file("tif_close_mapped_test.tif");
  DiagnosticRecorder diagnostics;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  diagnostics.Install(opts.get());

  {
    UniqueTiff writer(TIFFOpenExt(temp_file.c_str(), "w", opts.get()));
    ASSERT_NE(writer, nullptr);
    WriteRgbImage(writer.get(), 6, 4, 31);
  }

  UniqueTiff reader(TIFFOpenExt(temp_file.c_str(), "rM", opts.get()));
  ASSERT_NE(reader, nullptr);
  EXPECT_NE(reader->tif_flags & TIFF_MAPPED, 0U);
  VerifyRgbImage(reader.get(), 6, 4, 31);

  reader.reset();
  EXPECT_EQ(diagnostics.error_count(), 0);
}

TEST(TifCloseTest, CloseWriteHandleFlushesBufferedScanlinesAndDirectory) {
  struct ModeCase {
    std::string_view mode;
    bool expect_big_endian;
    bool expect_big_tiff;
    uint8_t seed;
  };
  constexpr std::array<ModeCase, 5> kCases = {{
      {.mode = "wl",
       .expect_big_endian = false,
       .expect_big_tiff = false,
       .seed = 3},
      {.mode = "wb",
       .expect_big_endian = true,
       .expect_big_tiff = false,
       .seed = 41},
      {.mode = "w8l",
       .expect_big_endian = false,
       .expect_big_tiff = true,
       .seed = 77},
      {.mode = "w8b",
       .expect_big_endian = true,
       .expect_big_tiff = true,
       .seed = 113},
      {.mode = "a",
       .expect_big_endian = false,
       .expect_big_tiff = false,
       .seed = 155},
  }};

  for (const ModeCase& tc : kCases) {
    SCOPED_TRACE(tc.mode);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(tc.mode.data());
      ASSERT_NE(writer, nullptr);
      EXPECT_EQ(TIFFIsBigEndian(writer.get()), tc.expect_big_endian ? 1 : 0);
      EXPECT_EQ(TIFFIsBigTIFF(writer.get()), tc.expect_big_tiff ? 1 : 0);
      // Do not call TIFFWriteDirectory; TIFFClose must flush data and IFD.
      WriteRgbImage(writer.get(), 5, 4, tc.seed);
    }
    EXPECT_TRUE(stream.is_closed());
    EXPECT_EQ(stream.close_calls(), 1);
    EXPECT_GT(stream.write_calls(), 0);
    EXPECT_EQ(stream.diagnostics().error_count(), 0);

    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFIsBigEndian(reader.get()), tc.expect_big_endian ? 1 : 0);
    EXPECT_EQ(TIFFIsBigTIFF(reader.get()), tc.expect_big_tiff ? 1 : 0);
    VerifyRgbImage(reader.get(), 5, 4, tc.seed);
    EXPECT_EQ(stream.diagnostics().error_count(), 0);
  }
}

TEST(TifCloseTest, CloseUpdateHandleFlushesAppendedDirectory) {
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    WriteRgbImage(writer.get(), 3, 2, 9);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  {
    UniqueTiff updater = stream.Open("r+");
    ASSERT_NE(updater, nullptr);
    EXPECT_EQ(TIFFSetDirectory(updater.get(), 0), 1);
    EXPECT_EQ(TIFFCreateDirectory(updater.get()), 0);
    WriteRgbImage(updater.get(), 4, 2, 88);
    // Close without calling TIFFWriteDirectory explicitly.
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0);

  UniqueTiff reader = stream.Open("rm");
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(TIFFNumberOfDirectories(reader.get()), 2);
  ASSERT_EQ(TIFFSetDirectory(reader.get(), 0), 1);
  VerifyRgbImage(reader.get(), 3, 2, 9);
  ASSERT_EQ(TIFFSetDirectory(reader.get(), 1), 1);
  VerifyRgbImage(reader.get(), 4, 2, 88);
}

TEST(TifCloseTest, CloseCompressedWriteHandleFlushesAndCleansCodec) {
  constexpr std::array<uint16_t, 2> kCodecs = {
      COMPRESSION_LZW,
      COMPRESSION_ADOBE_DEFLATE,
  };
  for (uint16_t codec : kCodecs) {
    SCOPED_TRACE(codec);
    if (TIFFIsCODECConfigured(codec) == 0) continue;

    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      WriteRgbImage(writer.get(), 8, 4, 25, codec);
    }
    EXPECT_TRUE(stream.is_closed());
    EXPECT_EQ(stream.diagnostics().error_count(), 0);

    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    uint16_t actual_codec = 0;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_COMPRESSION, &actual_codec),
              1);
    EXPECT_EQ(actual_codec, codec);
    VerifyRgbImage(reader.get(), 8, 4, 25);
  }
}

TEST(TifCloseTest, CloseWithUserSuppliedBuffersDoesNotFreeCallerMemory) {
  MemoryTiffStream stream;
  std::vector<uint8_t> user_write_buf(4096, 0);
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(
        TIFFWriteBufferSetup(writer.get(), user_write_buf.data(),
                             static_cast<tmsize_t>(user_write_buf.size())),
        1);
    EXPECT_EQ(writer->tif_flags & TIFF_MYBUFFER, 0U);
    WriteRgbImage(writer.get(), 4, 3, 55);
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0);

  std::vector<uint8_t> user_read_buf(4096, 0);
  UniqueTiff reader = stream.Open("rm");
  ASSERT_NE(reader, nullptr);
  ASSERT_EQ(TIFFReadBufferSetup(reader.get(), user_read_buf.data(),
                                static_cast<tmsize_t>(user_read_buf.size())),
            1);
  EXPECT_NE(reader->tif_rawdata, nullptr);
  EXPECT_EQ(reader->tif_flags & TIFF_MYBUFFER, 0U);
  VerifyRgbImage(reader.get(), 4, 3, 55);

  reader.reset();
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
}

TEST(TifCloseTest, CleanupFlushesAndFreesWithoutCallingCloseProc) {
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    WriteRgbImage(writer.get(), 4, 2, 63);

    TIFF* raw_writer = writer.release();
    TIFFCloseProc closeproc = TIFFGetCloseProc(raw_writer);
    thandle_t clientdata = TIFFClientdata(raw_writer);
    ASSERT_NE(closeproc, nullptr);

    TIFFCleanup(raw_writer);
    // TIFFCleanup flushes data and frees TIFF*, but does not call closeproc.
    EXPECT_FALSE(stream.is_closed());
    EXPECT_EQ(stream.close_calls(), 0);
    EXPECT_EQ(closeproc(clientdata), 0);
    EXPECT_TRUE(stream.is_closed());
    EXPECT_EQ(stream.close_calls(), 1);
  }

  UniqueTiff reader = stream.Open("rm");
  ASSERT_NE(reader, nullptr);
  VerifyRgbImage(reader.get(), 4, 2, 63);

  TIFF* raw_reader = reader.release();
  TIFFCloseProc reader_close = TIFFGetCloseProc(raw_reader);
  thandle_t reader_data = TIFFClientdata(raw_reader);
  TIFFCleanup(raw_reader);
  EXPECT_FALSE(stream.is_closed());
  EXPECT_EQ(stream.close_calls(), 1);
  EXPECT_EQ(reader_close(reader_data), 0);
  EXPECT_TRUE(stream.is_closed());
  EXPECT_EQ(stream.close_calls(), 2);
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
}

TEST(TifCloseTest, CleanupOnFdHandleLeavesFdOpenUntilCloseProcCalled) {
  TempFile temp_file("tif_cleanup_fd_test.tif");
  DiagnosticRecorder diagnostics;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  diagnostics.Install(opts.get());

  const int fd = ::open(temp_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  ASSERT_GE(fd, 0);
  bool fd_owned = true;
  absl::Cleanup close_fd_guard = [&fd_owned, fd] {
    if (fd_owned && fd >= 0) ::close(fd);
  };

  UniqueTiff tif(TIFFFdOpenExt(fd, temp_file.c_str(), "w", opts.get()));
  ASSERT_NE(tif, nullptr);
  WriteRgbImage(tif.get(), 2, 2, 11);

  TIFF* raw_tif = tif.release();
  TIFFCloseProc closeproc = TIFFGetCloseProc(raw_tif);
  thandle_t clientdata = TIFFClientdata(raw_tif);
  ASSERT_NE(closeproc, nullptr);

  TIFFCleanup(raw_tif);
  EXPECT_NE(::fcntl(fd, F_GETFD), -1);
  EXPECT_EQ(closeproc(clientdata), 0);
  fd_owned = false;
  errno = 0;
  EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
  EXPECT_EQ(diagnostics.error_count(), 0);
}

TEST(TifCloseTest, ClientInfoLinksAreFreedOnClose) {
  MemoryTiffStream stream(BuildRawGrayscaleTiffWithUnknownTags(false, false));
  DiagnosticRecorder diagnostics;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  diagnostics.Install(opts.get());
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

  UniqueTiff tif = stream.Open("rm", opts.get());
  ASSERT_NE(tif, nullptr);

  int payload1 = 101;
  int payload2 = 202;
  int payload2_updated = 203;
  int payload3 = 303;
  TIFFSetClientInfo(tif.get(), &payload1, "alpha");
  TIFFSetClientInfo(tif.get(), &payload2, "beta");
  TIFFSetClientInfo(tif.get(), &payload3, "gamma");
  TIFFSetClientInfo(tif.get(), &payload2_updated, "beta");

  EXPECT_EQ(TIFFGetClientInfo(tif.get(), "alpha"), &payload1);
  EXPECT_EQ(TIFFGetClientInfo(tif.get(), "beta"), &payload2_updated);
  EXPECT_EQ(TIFFGetClientInfo(tif.get(), "gamma"), &payload3);
  EXPECT_EQ(TIFFGetClientInfo(tif.get(), "missing"), nullptr);

  tif.reset();
  EXPECT_TRUE(stream.is_closed());
  EXPECT_EQ(diagnostics.error_count(), 0);
}

TEST(TifCloseTest, AnonymousAndMergedCustomFieldsAreFreedOnClose) {
  for (bool big_tiff : {false, true}) {
    SCOPED_TRACE(big_tiff);
    MemoryTiffStream stream(
        BuildRawGrayscaleTiffWithUnknownTags(big_tiff, big_tiff));
    DiagnosticRecorder diagnostics;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    diagnostics.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    UniqueTiff tif = stream.Open("rm", opts.get());
    ASSERT_NE(tif, nullptr);

    const TIFFField* anon_long_fld =
        TIFFFindField(tif.get(), kAnonLongTag, TIFF_ANY);
    ASSERT_NE(anon_long_fld, nullptr);
    EXPECT_EQ(TIFFFieldIsAnonymous(anon_long_fld), 1);

    const TIFFField* anon_ascii_fld =
        TIFFFindField(tif.get(), kAnonAsciiTag, TIFF_ANY);
    ASSERT_NE(anon_ascii_fld, nullptr);
    EXPECT_EQ(TIFFFieldIsAnonymous(anon_ascii_fld), 1);

    const TIFFField* anon_short_fld =
        TIFFFindField(tif.get(), kAnonShortArrayTag, TIFF_ANY);
    ASSERT_NE(anon_short_fld, nullptr);
    EXPECT_EQ(TIFFFieldIsAnonymous(anon_short_fld), 1);

    const TIFFField* artist_fld =
        TIFFFindField(tif.get(), TIFFTAG_ARTIST, TIFF_ANY);
    ASSERT_NE(artist_fld, nullptr);
    EXPECT_EQ(TIFFFieldIsAnonymous(artist_fld), 0);

    uint32_t count = 0;
    const void* raw_val = nullptr;
    ASSERT_EQ(TIFFGetField(tif.get(), kAnonLongTag, &count, &raw_val), 1);
    EXPECT_EQ(count, 1U);
    ASSERT_NE(raw_val, nullptr);
    EXPECT_EQ(*static_cast<const uint32_t*>(raw_val), 0x12345678U);

    uint32_t ascii_count = 0;
    const void* ascii_val = nullptr;
    ASSERT_EQ(TIFFGetField(tif.get(), kAnonAsciiTag, &ascii_count, &ascii_val),
              1);
    EXPECT_EQ(ascii_count, 11U);
    ASSERT_NE(ascii_val, nullptr);
    EXPECT_STREQ(static_cast<const char*>(ascii_val), "anon_value");

    uint32_t short_count = 0;
    const void* short_val = nullptr;
    ASSERT_EQ(
        TIFFGetField(tif.get(), kAnonShortArrayTag, &short_count, &short_val),
        1);
    EXPECT_EQ(short_count, 3U);
    ASSERT_NE(short_val, nullptr);
    const auto* shorts = static_cast<const uint16_t*>(short_val);
    EXPECT_EQ(shorts[0], 11);
    EXPECT_EQ(shorts[1], 22);
    EXPECT_EQ(shorts[2], 33);

    // Merge two compat field arrays to exercise tif_fieldscompat reallocation
    // and cleanup.
    static const TIFFFieldInfo kInfo1[] = {
        {kMergedLongTag, 1, 1, TIFF_LONG, FIELD_CUSTOM, 1, 0, "MergedLong"},
    };
    static const TIFFFieldInfo kInfo2[] = {
        {kMergedAsciiTag, -1, -1, TIFF_ASCII, FIELD_CUSTOM, 1, 0,
         "MergedAscii"},
    };
    EXPECT_EQ(TIFFMergeFieldInfo(tif.get(), kInfo1, 1), 0);
    EXPECT_EQ(TIFFMergeFieldInfo(tif.get(), kInfo2, 1), 0);
    EXPECT_EQ(tif->tif_nfieldscompat, 2U);

    EXPECT_EQ(TIFFSetField(tif.get(), kMergedLongTag, uint32_t{999}), 1);
    uint32_t merged_got = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), kMergedLongTag, &merged_got), 1);
    EXPECT_EQ(merged_got, 999U);

    // Trigger an allocated_size == 0 entry in tif_fieldscompat to cover both
    // branches of the compat cleanup loop in TIFFCleanup.
    EXPECT_EQ(TIFFMergeFieldInfo(tif.get(), kInfo1, 0), -1);
    EXPECT_EQ(tif->tif_nfieldscompat, 3U);
    EXPECT_EQ(tif->tif_fieldscompat[2].allocated_size, 0U);
    EXPECT_EQ(diagnostics.error_count(), 2U);
    diagnostics.Clear();

    tif.reset();
    EXPECT_TRUE(stream.is_closed());
    EXPECT_EQ(diagnostics.error_count(), 0);
  }
}

TEST(TifCloseTest, TagExtenderPopulatedFieldsCompatCleanedUpOnClose) {
  ScopedTagExtender extender_guard(&CustomTagExtender);

  MemoryTiffStream stream;
  UniqueTiff writer = stream.Open("wl");
  ASSERT_NE(writer, nullptr);
  EXPECT_NE(TIFFFindField(writer.get(), kExtenderTag, TIFF_ANY), nullptr);
  EXPECT_EQ(writer->tif_nfieldscompat, 1U);

  WriteRgbImage(writer.get(), 2, 2, 7);
  EXPECT_EQ(TIFFWriteDirectory(writer.get()), 1);
  // Creating a directory refreshes tif_fieldscompat in TIFFDefaultDirectory.
  EXPECT_EQ(TIFFCreateDirectory(writer.get()), 0);
  EXPECT_EQ(writer->tif_nfieldscompat, 1U);
  WriteRgbImage(writer.get(), 2, 2, 14);

  writer.reset();
  EXPECT_TRUE(stream.is_closed());
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
}

TEST(TifCloseTest, CleanupIfdOffsetAndNumberMapsDirectlyAndOnClose) {
  for (bool big_tiff : {false, true}) {
    SCOPED_TRACE(big_tiff);
    MemoryTiffStream stream(BuildMultiIfdRawTiff(big_tiff, big_tiff, 3));
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);

    EXPECT_EQ(TIFFNumberOfDirectories(tif.get()), 3);
    EXPECT_EQ(TIFFSetDirectory(tif.get(), 2), 1);
    EXPECT_EQ(TIFFSetDirectory(tif.get(), 0), 1);
    EXPECT_NE(tif->tif_map_dir_offset_to_number, nullptr);
    EXPECT_NE(tif->tif_map_dir_number_to_offset, nullptr);

    _TIFFCleanupIFDOffsetAndNumberMaps(tif.get());
    EXPECT_EQ(tif->tif_map_dir_offset_to_number, nullptr);
    EXPECT_EQ(tif->tif_map_dir_number_to_offset, nullptr);

    // Calling again when maps are already nullptr must be a safe no-op.
    _TIFFCleanupIFDOffsetAndNumberMaps(tif.get());
    EXPECT_EQ(tif->tif_map_dir_offset_to_number, nullptr);
    EXPECT_EQ(tif->tif_map_dir_number_to_offset, nullptr);

    // Repopulate the maps and let TIFFClose clean them up automatically.
    EXPECT_EQ(TIFFSetDirectory(tif.get(), 1), 1);
    EXPECT_NE(tif->tif_map_dir_offset_to_number, nullptr);
    EXPECT_NE(tif->tif_map_dir_number_to_offset, nullptr);

    tif.reset();
    EXPECT_TRUE(stream.is_closed());
    EXPECT_EQ(stream.diagnostics().error_count(), 0);
  }
}

TEST(TifCloseTest, EarlyOpenFailureCleansUpWithNullFieldsTable) {
  // Suppress global handlers for patched tif_dirread.c paths that log via
  // TIFFErrorExt() instead of TIFFErrorExtR().
  ScopedTiffErrorHandlers silence_global(nullptr, nullptr);
  const std::array<std::vector<uint8_t>, 5> kInvalidBuffers = {{
      // Empty buffer: fails header read before tif_fields is initialized.
      {},
      // Truncated ClassicTIFF header.
      {'I', 'I', 42, 0},
      // Bad magic bytes.
      {'X', 'Y', 42, 0, 8, 0, 0, 0},
      // Bad BigTIFF offset size (4 instead of 8).
      {'I', 'I', 43, 0, 4, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0},
      // ClassicTIFF with dircount == 0 IFD (fails inside TIFFReadDirectory).
      {'I', 'I', 42, 0, 8, 0, 0, 0, 0, 0},
  }};

  for (size_t i = 0; i < kInvalidBuffers.size(); ++i) {
    SCOPED_TRACE(i);
    MemoryTiffStream stream(kInvalidBuffers[i]);
    UniqueTiff tif = stream.Open("rm");
    EXPECT_EQ(tif, nullptr);
    EXPECT_GT(stream.diagnostics().error_count(), 0);
  }
}

TEST(TifCloseTest, AllocLimitFailureRecoveryClosesWithoutLeaks) {
  MemoryTiffStream stream(BuildRawGrayscaleTiffWithUnknownTags(false, false));
  DiagnosticRecorder diagnostics;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  diagnostics.Install(opts.get());
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

  UniqueTiff tif = stream.Open("rm", opts.get());
  ASSERT_NE(tif, nullptr);

  {
    ScopedSingleMemAllocLimit single_limit(tif.get(), 64);
    EXPECT_EQ(TIFFReadBufferSetup(tif.get(), nullptr, 4096), 0);
    EXPECT_EQ(tif->tif_rawdata, nullptr);
  }
  {
    ScopedCumulatedMemAllocRemaining cum_limit(tif.get(), 16);
    EXPECT_EQ(TIFFReadBufferSetup(tif.get(), nullptr, 4096), 0);
    EXPECT_EQ(tif->tif_rawdata, nullptr);
  }
  EXPECT_EQ(diagnostics.error_count(), 4U);
  diagnostics.Clear();

  tif.reset();
  EXPECT_TRUE(stream.is_closed());
  EXPECT_EQ(diagnostics.error_count(), 0);
}

TEST(TifCloseTest, NonZeroCumulatedMemAllocOnCleanupEmitsError) {
  MemoryTiffStream stream(BuildRawGrayscaleTiffWithUnknownTags(false, false));
  DiagnosticRecorder diagnostics;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  diagnostics.Install(opts.get());
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

  UniqueTiff tif = stream.Open("rm", opts.get());
  ASSERT_NE(tif, nullptr);

  // Adjust the integer counter without leaking heap allocations so LSan stays
  // clean while exercising TIFFCleanup's non-zero accounting diagnostic.
  tif->tif_cur_cumulated_mem_alloc += 64;
  tif.reset();

  EXPECT_TRUE(stream.is_closed());
  const std::vector<DiagnosticRecord> errors = diagnostics.errors();
  ASSERT_EQ(errors.size(), 1U);
  EXPECT_EQ(errors[0].module, "TIFFCleanup");
  EXPECT_THAT(errors[0].message, HasSubstr("tif_cur_cumulated_mem_alloc"));
  EXPECT_THAT(errors[0].message, HasSubstr("64"));
}

TEST(TifCloseTest, CloseIgnoresCloseProcErrorReturn) {
  MemoryTiffStream stream;
  UniqueTiff writer = stream.Open("wl");
  ASSERT_NE(writer, nullptr);
  WriteRgbImage(writer.get(), 2, 2, 13);

  stream.ForceCloseReturn(-1);
  writer.reset();
  EXPECT_TRUE(stream.is_closed());
  EXPECT_EQ(stream.close_calls(), 1);
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
}

}  // namespace
}  // namespace libtiff
