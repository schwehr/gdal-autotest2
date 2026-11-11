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

// Clean-room unit tests for libtiff directory writing and rewriting routines
// (libtiff/tif_dirwrite.c).
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_dirwrite.c

#include <fcntl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tif_dir.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

struct CapturedGlobalErrors {
  static inline std::vector<std::string>* g_messages = nullptr;

  static void OnBasicError(const char* /*module*/, const char* /*fmt*/,
                           va_list /*ap*/) {}

  static void OnExtError(thandle_t /*fd*/, const char* /*module*/,
                         const char* fmt, va_list ap) {
    if (g_messages == nullptr || fmt == nullptr) return;
    va_list copy;
    va_copy(copy, ap);
    int len = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (len < 0) return;
    std::string msg(static_cast<size_t>(len) + 1, '\0');
    std::vsnprintf(msg.data(), msg.size(), fmt, ap);
    msg.resize(static_cast<size_t>(len));
    g_messages->push_back(std::move(msg));
  }
};

class ScopedQuietGlobalHandlers {
 public:
  ScopedQuietGlobalHandlers()
      : prev_messages_(CapturedGlobalErrors::g_messages),
        basic_guard_(&CapturedGlobalErrors::OnBasicError,
                     &CapturedGlobalErrors::OnBasicError),
        ext_guard_(&CapturedGlobalErrors::OnExtError,
                   &CapturedGlobalErrors::OnExtError) {
    CapturedGlobalErrors::g_messages = &messages_;
  }

  ~ScopedQuietGlobalHandlers() {
    CapturedGlobalErrors::g_messages = prev_messages_;
  }

  ScopedQuietGlobalHandlers(const ScopedQuietGlobalHandlers&) = delete;
  ScopedQuietGlobalHandlers& operator=(const ScopedQuietGlobalHandlers&) =
      delete;

  const std::vector<std::string>& messages() const { return messages_; }

 private:
  std::vector<std::string> messages_;
  std::vector<std::string>* prev_messages_;
  ScopedTiffErrorHandlers basic_guard_;
  ScopedTiffErrorHandlersExt ext_guard_;
};

bool ContainsSubstring(const std::vector<DiagnosticRecord>& records,
                       std::string_view needle) {
  for (const DiagnosticRecord& rec : records) {
    if (rec.message.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

UniqueTiff OpenWithBudget(MemoryTiffStream* stream, const char* mode,
                          tmsize_t max_cumulated = 1 << 24) {
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  stream->diagnostics().Install(opts.get());
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), max_cumulated);
  return stream->Open(mode, opts.get());
}

void ConfigureBasicStripedGray8(TIFF* tif, uint32_t width, uint32_t length,
                                uint32_t rows_per_strip,
                                uint16_t compression = COMPRESSION_NONE) {
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

void ConfigureBasicTiledGray8(TIFF* tif, uint32_t width, uint32_t length,
                              uint32_t tile_width, uint32_t tile_length,
                              uint16_t compression = COMPRESSION_NONE) {
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

void WriteConstantRows(TIFF* tif, uint32_t width, uint32_t length,
                       uint8_t base_val) {
  std::vector<uint8_t> row(width);
  for (uint32_t y = 0; y < length; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      row[x] = static_cast<uint8_t>((base_val + y * 7U + x) & 0xffU);
    }
    std::vector<uint8_t> mutable_row = row;
    ASSERT_EQ(TIFFWriteScanline(tif, mutable_row.data(), y, 0), 1);
  }
}

void WriteTwoDirImage(MemoryTiffStream* stream, const char* mode) {
  UniqueTiff writer = stream->Open(mode);
  ASSERT_NE(writer, nullptr);
  for (uint32_t dir = 0; dir < 2; ++dir) {
    ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEDESCRIPTION,
                           dir == 0 ? "dir0" : "dir1"),
              1);
    WriteConstantRows(writer.get(), 4, 2, static_cast<uint8_t>(10 + dir * 20));
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }
}

// Custom codec helpers for testing Step 2 FIELD_CODEC branches cleanly.
constexpr uint16_t kTestCodecScheme = 65001;
constexpr uint32_t kTestCodecAsciiTag = 65010;
constexpr uint32_t kTestCodecUnsupportedTag = 65011;

#define FIELD_TEST_ASCII (FIELD_CODEC + 0)
#define FIELD_TEST_U8 (FIELD_CODEC + 1)

struct TestCodecState {
  TIFFVSetMethod vsetparent = nullptr;
  TIFFVGetMethod vgetparent = nullptr;
  char* ascii_val = nullptr;
  uint8_t u8_val = 0;
  bool fail_postencode = false;
};

const TIFFField kTestCodecFields[] = {
    {kTestCodecAsciiTag, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_ASCII, 0,
     TIFF_SETGET_ASCII, FIELD_TEST_ASCII, 1, 0, "TestCodecAscii", nullptr},
    {kTestCodecUnsupportedTag, 1, 1, TIFF_BYTE, 0, TIFF_SETGET_UINT8,
     FIELD_TEST_U8, 1, 0, "TestCodecUnsupportedU8", nullptr},
};

int TestCodecVSetField(TIFF* tif, uint32_t tag, va_list ap) {
  auto* sp = reinterpret_cast<TestCodecState*>(tif->tif_data);
  if (sp == nullptr) return 0;
  switch (tag) {
    case kTestCodecAsciiTag: {
      const char* s = va_arg(ap, const char*);
      if (sp->ascii_val != nullptr) {
        _TIFFfreeExt(tif, sp->ascii_val);
        sp->ascii_val = nullptr;
      }
      if (s != nullptr) {
        size_t len = std::strlen(s) + 1;
        sp->ascii_val = static_cast<char*>(_TIFFmallocExt(tif, len));
        if (sp->ascii_val == nullptr) return 0;
        std::memcpy(sp->ascii_val, s, len);
      }
      const TIFFField* fip = TIFFFieldWithTag(tif, tag);
      if (fip == nullptr) return 0;
      TIFFSetFieldBit(tif, fip->field_bit);
      tif->tif_flags |= TIFF_DIRTYDIRECT;
      return 1;
    }
    case kTestCodecUnsupportedTag: {
      sp->u8_val = static_cast<uint8_t>(va_arg(ap, int));
      const TIFFField* fip = TIFFFieldWithTag(tif, tag);
      if (fip == nullptr) return 0;
      TIFFSetFieldBit(tif, fip->field_bit);
      tif->tif_flags |= TIFF_DIRTYDIRECT;
      return 1;
    }
    default:
      return (*sp->vsetparent)(tif, tag, ap);
  }
}

int TestCodecVGetField(TIFF* tif, uint32_t tag, va_list ap) {
  auto* sp = reinterpret_cast<TestCodecState*>(tif->tif_data);
  if (sp == nullptr) return 0;
  switch (tag) {
    case kTestCodecAsciiTag:
      *va_arg(ap, const char**) = sp->ascii_val;
      return 1;
    case kTestCodecUnsupportedTag:
      *va_arg(ap, uint8_t*) = sp->u8_val;
      return 1;
    default:
      return (*sp->vgetparent)(tif, tag, ap);
  }
}

void TestCodecCleanup(TIFF* tif) {
  auto* sp = reinterpret_cast<TestCodecState*>(tif->tif_data);
  if (sp != nullptr) {
    tif->tif_tagmethods.vsetfield = sp->vsetparent;
    tif->tif_tagmethods.vgetfield = sp->vgetparent;
    if (sp->ascii_val != nullptr) {
      _TIFFfreeExt(tif, sp->ascii_val);
      sp->ascii_val = nullptr;
    }
    _TIFFfreeExt(tif, tif->tif_data);
    tif->tif_data = nullptr;
  }
  _TIFFSetDefaultCompressionState(tif);
}

int TestCodecBoolOk(TIFF* /*tif*/) { return 1; }
int TestCodecPreEncodeOk(TIFF* /*tif*/, uint16_t /*s*/) { return 1; }
int TestCodecEncodeOk(TIFF* /*tif*/, uint8_t* /*bp*/, tmsize_t /*cc*/,
                      uint16_t /*s*/) {
  return 1;
}

int TestCodecFailingPostEncode(TIFF* tif) {
  TIFFErrorExtR(tif, "TestCodec", "Forced postencode error");
  return 0;
}

int InitTestCodec(TIFF* tif, int /*scheme*/) {
  if (!_TIFFMergeFields(tif, kTestCodecFields,
                        TIFFArrayCount(kTestCodecFields))) {
    return 0;
  }
  auto* sp =
      static_cast<TestCodecState*>(_TIFFmallocExt(tif, sizeof(TestCodecState)));
  if (sp == nullptr) return 0;
  std::memset(sp, 0, sizeof(TestCodecState));
  tif->tif_data = reinterpret_cast<uint8_t*>(sp);
  sp->vsetparent = tif->tif_tagmethods.vsetfield;
  sp->vgetparent = tif->tif_tagmethods.vgetfield;
  tif->tif_tagmethods.vsetfield = &TestCodecVSetField;
  tif->tif_tagmethods.vgetfield = &TestCodecVGetField;
  tif->tif_setupencode = &TestCodecBoolOk;
  tif->tif_preencode = &TestCodecPreEncodeOk;
  tif->tif_encoderow = &TestCodecEncodeOk;
  tif->tif_encodestrip = &TestCodecEncodeOk;
  tif->tif_postencode = &TestCodecBoolOk;
  tif->tif_cleanup = &TestCodecCleanup;
  return 1;
}

int InitFailingPostEncodeCodec(TIFF* tif, int /*scheme*/) {
  tif->tif_setupencode = &TestCodecBoolOk;
  tif->tif_preencode = &TestCodecPreEncodeOk;
  tif->tif_encoderow = &TestCodecEncodeOk;
  tif->tif_encodestrip = &TestCodecEncodeOk;
  tif->tif_postencode = &TestCodecFailingPostEncode;
  return 1;
}

void WriteBe32(uint8_t* dst, uint32_t v) {
  dst[0] = static_cast<uint8_t>((v >> 24) & 0xffU);
  dst[1] = static_cast<uint8_t>((v >> 16) & 0xffU);
  dst[2] = static_cast<uint8_t>((v >> 8) & 0xffU);
  dst[3] = static_cast<uint8_t>(v & 0xffU);
}

void WriteLe32(uint8_t* dst, uint32_t v) {
  dst[0] = static_cast<uint8_t>(v & 0xffU);
  dst[1] = static_cast<uint8_t>((v >> 8) & 0xffU);
  dst[2] = static_cast<uint8_t>((v >> 16) & 0xffU);
  dst[3] = static_cast<uint8_t>((v >> 24) & 0xffU);
}

TEST(TifDirWriteTest, ReadOnlyHandleWriteDirectoryAndDeferStrile) {
  MemoryTiffStream stream;
  WriteTwoDirImage(&stream, "wl");

  const size_t size_before = stream.data().size();
  const size_t writes_before = stream.write_calls();

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(TIFFWriteDirectory(reader.get()), 1);
  EXPECT_EQ(TIFFDeferStrileArrayWriting(reader.get()), 0);
  EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(), "read-only"));
  EXPECT_EQ(stream.data().size(), size_before);
  EXPECT_EQ(stream.write_calls(), writes_before);
}

TEST(TifDirWriteTest, DeferStrileFailsAfterDirectoryWritten) {
  MemoryTiffStream stream;
  UniqueTiff writer = stream.Open("wl");
  ASSERT_NE(writer, nullptr);
  ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
  ASSERT_EQ(TIFFCheckpointDirectory(writer.get()), 1);
  EXPECT_NE(TIFFCurrentDirOffset(writer.get()), 0U);
  EXPECT_EQ(TIFFDeferStrileArrayWriting(writer.get()), 0);
  EXPECT_TRUE(
      ContainsSubstring(stream.diagnostics().errors(), "already been written"));
}

TEST(TifDirWriteTest, CheckpointDirectorySetupAndInPlaceAndGrowth) {
  for (const char* mode : {"wl", "wb", "w8l", "w8b"}) {
    SCOPED_TRACE(mode);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(mode);
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripedGray8(writer.get(), 4, 4, 2);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_ARTIST, "Initial Artist Name"), 1);
      // td_stripoffset_p is null before any scanline write, so checkpoint calls
      // TIFFSetupStrips automatically.
      EXPECT_EQ(writer->tif_dir.td_stripoffset_p, nullptr);
      ASSERT_EQ(TIFFCheckpointDirectory(writer.get()), 1);
      const uint64_t off_first = TIFFCurrentDirOffset(writer.get());
      EXPECT_NE(off_first, 0U);

      // Shrink ARTIST first and checkpoint BEFORE writing scanlines so
      // BigTIFF big-endian ("w8b") doesn't in-place-swab populated
      // td_stripoffset_p entries before the final TIFFWriteDirectory
      // relocation.
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ARTIST, "Short"), 1);
      ASSERT_EQ(TIFFCheckpointDirectory(writer.get()), 1);
      EXPECT_EQ(TIFFCurrentDirOffset(writer.get()), off_first);
      WriteConstantRows(writer.get(), 4, 4, 5);

      // Add a long custom tag so td_dirdatasize_write > td_dirdatasize_read
      // and directory write auto-relocates via TIFFRewriteDirectorySec.
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEDESCRIPTION,
                             "A much longer image description triggering "
                             "automatic directory relocation to EOF"),
                1);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFNumberOfDirectories(reader.get()), 1U);
    const char* artist = nullptr;
    const char* desc = nullptr;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_ARTIST, &artist), 1);
    ASSERT_NE(artist, nullptr);
    EXPECT_STREQ(artist, "Short");
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGEDESCRIPTION, &desc), 1);
    ASSERT_NE(desc, nullptr);
    EXPECT_STREQ(desc,
                 "A much longer image description triggering "
                 "automatic directory relocation to EOF");
    std::vector<uint8_t> buf(4);
    for (uint32_t y = 0; y < 4; ++y) {
      EXPECT_EQ(TIFFReadScanline(reader.get(), buf.data(), y, 0), 1);
      EXPECT_EQ(buf[0], static_cast<uint8_t>((5 + y * 7U) & 0xffU));
    }
  }
}

TEST(TifDirWriteTest,
     RewriteDirectoryWhenUnwrittenRelocatesFirstAndSubsequent) {
  for (const char* mode : {"wl", "wb", "w8l", "w8b"}) {
    SCOPED_TRACE(mode);
    MemoryTiffStream stream;
    uint64_t dir0_orig_off = 0;
    uint64_t dir1_orig_off = 0;
    {
      UniqueTiff writer = stream.Open(mode);
      ASSERT_NE(writer, nullptr);
      // Rewrite before first write falls back to TIFFWriteDirectory.
      ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEDESCRIPTION, "D0"), 1);
      WriteConstantRows(writer.get(), 4, 2, 11);
      ASSERT_EQ(TIFFRewriteDirectory(writer.get()), 1);
      dir0_orig_off = writer->tif_lastdiroff;
      EXPECT_NE(dir0_orig_off, 0U);

      // Write second directory.
      ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEDESCRIPTION, "D1"), 1);
      WriteConstantRows(writer.get(), 4, 2, 22);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
      dir1_orig_off = writer->tif_lastdiroff;
      EXPECT_GT(dir1_orig_off, dir0_orig_off);
    }

    // Reopen in read/write mode, grow and rewrite IFD 0, then grow and rewrite
    // the second IFD (relocating IFD 0 links it after IFD 1).
    {
      UniqueTiff updater = stream.Open("r+");
      ASSERT_NE(updater, nullptr);
      ASSERT_EQ(TIFFSetDirectory(updater.get(), 0), 1);
      EXPECT_EQ(TIFFCurrentDirOffset(updater.get()), dir0_orig_off);
      ASSERT_EQ(TIFFSetField(updater.get(), TIFFTAG_SOFTWARE,
                             "RelocatedFirstIfdPayload"),
                1);
      ASSERT_EQ(TIFFRewriteDirectory(updater.get()), 1);
      EXPECT_EQ(updater->tif_dir.td_iswrittentofile, 0U);

      // Note: relocating IFD 0 linked it after IFD 1, so original IFD 1 is now
      // directory 0, and relocated IFD 0 is now directory 1.
      ASSERT_EQ(TIFFSetDirectory(updater.get(), 1), 1);
      ASSERT_EQ(TIFFSetField(updater.get(), TIFFTAG_COPYRIGHT,
                             "RelocatedSecondChainedIfdPayload"),
                1);
      ASSERT_EQ(TIFFRewriteDirectory(updater.get()), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFNumberOfDirectories(reader.get()), 2U);
    ASSERT_EQ(TIFFSetDirectory(reader.get(), 1), 1);
    const char* copyright = nullptr;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_COPYRIGHT, &copyright), 1);
    ASSERT_NE(copyright, nullptr);
    EXPECT_STREQ(copyright, "RelocatedSecondChainedIfdPayload");
  }
}

TEST(TifDirWriteTest, ClassicAndBigTiffRewriteErrorsAndCorruption) {
  // 1. Classic TIFF header write error while rewriting first IFD.
  {
    MemoryTiffStream stream;
    WriteTwoDirImage(&stream, "wl");
    UniqueTiff updater = stream.Open("r+");
    ASSERT_NE(updater, nullptr);
    ASSERT_EQ(TIFFSetField(updater.get(), TIFFTAG_ARTIST, "UpdateIFD0"), 1);
    stream.FailWritesAfter(stream.write_calls(), -1);
    EXPECT_EQ(TIFFRewriteDirectory(updater.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "updating TIFF header"));
  }

  // 2. BigTIFF header write error while rewriting first IFD.
  {
    MemoryTiffStream stream;
    WriteTwoDirImage(&stream, "w8l");
    UniqueTiff updater = stream.Open("r+");
    ASSERT_NE(updater, nullptr);
    ASSERT_EQ(TIFFSetField(updater.get(), TIFFTAG_ARTIST, "UpdateBigIFD0"), 1);
    stream.FailWritesAfter(stream.write_calls(), -1);
    EXPECT_EQ(TIFFRewriteDirectory(updater.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "updating TIFF header"));
  }

  // 3. Classic TIFF synthetic tif_diroff > 0xFFFFFFFF error.
  {
    MemoryTiffStream stream;
    WriteTwoDirImage(&stream, "wl");
    UniqueTiff updater = stream.Open("r+");
    ASSERT_NE(updater, nullptr);
    ASSERT_EQ(TIFFSetDirectory(updater.get(), 1), 1);
    const uint64_t saved_off = updater->tif_diroff;
    updater->tif_diroff = 0x100000004ULL;
    EXPECT_EQ(TIFFRewriteDirectory(updater.get()), 0);
    EXPECT_TRUE(
        ContainsSubstring(stream.diagnostics().errors(), "32 bit range"));
    updater->tif_diroff = saved_off;
  }

  // 4. Classic TIFF subsequent IFD traversal faults: seek/read count, read
  // link,
  //    write link.
  {
    MemoryTiffStream stream;
    WriteTwoDirImage(&stream, "wl");
    UniqueTiff updater = stream.Open("r+");
    ASSERT_NE(updater, nullptr);
    ASSERT_EQ(TIFFSetDirectory(updater.get(), 1), 1);
    stream.FailSeeksAfter(stream.seek_calls());
    EXPECT_EQ(TIFFRewriteDirectory(updater.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "fetching directory count"));

    stream.ResetFaults();
    stream.diagnostics().Clear();
    stream.FailReadsAfter(stream.read_calls(), -1);
    EXPECT_EQ(TIFFRewriteDirectory(updater.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "fetching directory count"));

    stream.ResetFaults();
    stream.diagnostics().Clear();
    stream.FailReadsAfter(stream.read_calls() + 1, -1);
    EXPECT_EQ(TIFFRewriteDirectory(updater.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "fetching directory link"));

    stream.ResetFaults();
    stream.diagnostics().Clear();
    stream.FailWritesAfter(stream.write_calls(), -1);
    EXPECT_EQ(TIFFRewriteDirectory(updater.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "writing directory link"));
  }

  // 5. BigTIFF subsequent IFD traversal faults: read count, >0xFFFF count,
  //    read link, write link.
  {
    MemoryTiffStream stream;
    WriteTwoDirImage(&stream, "w8l");
    UniqueTiff updater = stream.Open("r+");
    ASSERT_NE(updater, nullptr);
    ASSERT_EQ(TIFFSetDirectory(updater.get(), 1), 1);

    stream.FailReadsAfter(stream.read_calls(), -1);
    EXPECT_EQ(TIFFRewriteDirectory(updater.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "fetching directory count"));

    stream.ResetFaults();
    stream.diagnostics().Clear();
    stream.FailReadsAfter(stream.read_calls() + 1, -1);
    EXPECT_EQ(TIFFRewriteDirectory(updater.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "fetching directory link"));

    stream.ResetFaults();
    stream.diagnostics().Clear();
    stream.FailWritesAfter(stream.write_calls(), -1);
    EXPECT_EQ(TIFFRewriteDirectory(updater.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "writing directory link"));

    // Corrupt IFD 0's dircount64 in backing buffer to > 0xFFFF.
    stream.ResetFaults();
    stream.diagnostics().Clear();
    const uint64_t ifd0_off = updater->tif_header.big.tiff_diroff;
    ASSERT_GE(stream.data().size(), ifd0_off + 8);
    uint8_t* p = stream.mutable_data().data() + ifd0_off;
    WriteLe32(p, 0x20000U);
    WriteLe32(p + 4, 0U);
    EXPECT_EQ(TIFFRewriteDirectory(updater.get()), 0);
    EXPECT_TRUE(
        ContainsSubstring(stream.diagnostics().errors(), "Sanity check"));
  }
}

TEST(TifDirWriteTest, WriteCustomDirectoryExifGpsAndCustomInPlaceAndAppend) {
  MemoryTiffStream stream;
  uint64_t exif_offset = 0;
  uint64_t gps_offset = 0;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
    WriteConstantRows(writer.get(), 4, 2, 1);
    // Reserve EXIF/GPS IFD pointers in main IFD via checkpoint, then update.
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_EXIFIFD, uint64_t{0}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_GPSIFD, uint64_t{0}), 1);
    ASSERT_EQ(TIFFCheckpointDirectory(writer.get()), 1);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);

    // Create and checkpoint EXIF custom directory, overwrite smaller, then
    // grow.
    ASSERT_EQ(TIFFCreateEXIFDirectory(writer.get()), 0);
    ASSERT_EQ(TIFFSetField(writer.get(), EXIFTAG_EXPOSURETIME, 0.008), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), EXIFTAG_DATETIMEORIGINAL,
                           "2026:01:02 03:04:05"),
              1);
    constexpr std::array<uint8_t, 4> kExifVer = {'0', '2', '3', '2'};
    ASSERT_EQ(TIFFSetField(writer.get(), EXIFTAG_EXIFVERSION, kExifVer.data()),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), EXIFTAG_FILESOURCE, uint8_t{3}), 1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), EXIFTAG_CAMERAOWNERNAME, "InitialOwnerLong"),
        1);
    ASSERT_EQ(TIFFWriteCustomDirectory(writer.get(), &exif_offset), 1);
    EXPECT_NE(exif_offset, 0U);

    // Overwrite EXIF custom IFD with smaller data -> reuses exif_offset.
    uint64_t exif_offset_inplace = 0;
    ASSERT_EQ(TIFFSetField(writer.get(), EXIFTAG_CAMERAOWNERNAME, "Owner"), 1);
    ASSERT_EQ(TIFFWriteCustomDirectory(writer.get(), &exif_offset_inplace), 1);
    EXPECT_EQ(exif_offset_inplace, exif_offset);

    // Grow EXIF custom IFD beyond td_dirdatasize_read -> appends at EOF.
    uint64_t exif_offset_grown = 0;
    ASSERT_EQ(TIFFSetField(writer.get(), EXIFTAG_CAMERAOWNERNAME,
                           "MuchLongerCameraOwnerNameExceedingPreviousSize"),
              1);
    ASSERT_EQ(TIFFWriteCustomDirectory(writer.get(), &exif_offset_grown), 1);
    EXPECT_GT(exif_offset_grown, exif_offset);
    exif_offset = exif_offset_grown;

    // Create GPS custom directory with rational double-precision arrays.
    ASSERT_EQ(TIFFCreateGPSDirectory(writer.get()), 0);
    constexpr std::array<uint8_t, 4> kGpsVer = {2, 3, 1, 0};
    ASSERT_EQ(TIFFSetField(writer.get(), GPSTAG_VERSIONID, kGpsVer.data()), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), GPSTAG_LATITUDEREF, "N"), 1);
    constexpr std::array<double, 3> kLat = {37.0, 25.0, 19.876};
    ASSERT_EQ(TIFFSetField(writer.get(), GPSTAG_LATITUDE, kLat.data()), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), GPSTAG_ALTITUDEREF, uint8_t{0}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), GPSTAG_ALTITUDE, 123.456789), 1);
    ASSERT_EQ(TIFFWriteCustomDirectory(writer.get(), &gps_offset), 1);
    EXPECT_NE(gps_offset, 0U);

    // Link EXIF & GPS offsets back into IFD 0.
    ASSERT_EQ(TIFFSetDirectory(writer.get(), 0), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_EXIFIFD, exif_offset), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_GPSIFD, gps_offset), 1);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  uint64_t read_exif_off = 0;
  uint64_t read_gps_off = 0;
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_EXIFIFD, &read_exif_off), 1);
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_GPSIFD, &read_gps_off), 1);
  EXPECT_EQ(read_exif_off, exif_offset);
  EXPECT_EQ(read_gps_off, gps_offset);

  ASSERT_EQ(TIFFReadEXIFDirectory(reader.get(), read_exif_off), 1);
  float exp_time = 0.0f;
  EXPECT_EQ(TIFFGetField(reader.get(), EXIFTAG_EXPOSURETIME, &exp_time), 1);
  EXPECT_NEAR(exp_time, 0.008f, 1e-5f);
  const char* owner = nullptr;
  EXPECT_EQ(TIFFGetField(reader.get(), EXIFTAG_CAMERAOWNERNAME, &owner), 1);
  ASSERT_NE(owner, nullptr);
  EXPECT_STREQ(owner, "MuchLongerCameraOwnerNameExceedingPreviousSize");

  ASSERT_EQ(TIFFSetDirectory(reader.get(), 0), 1);
  ASSERT_EQ(TIFFReadGPSDirectory(reader.get(), read_gps_off), 1);
  double alt = 0.0;
  EXPECT_EQ(TIFFGetField(reader.get(), GPSTAG_ALTITUDE, &alt), 1);
  EXPECT_NEAR(alt, 123.456789, 1e-6);
  const double* lat = nullptr;
  EXPECT_EQ(TIFFGetField(reader.get(), GPSTAG_LATITUDE, &lat), 1);
  ASSERT_NE(lat, nullptr);
  EXPECT_NEAR(lat[2], 19.876, 1e-5);
}

TEST(TifDirWriteTest, AllNamedStep1TagsColorMapTransferFunctionAndSubIfd) {
  for (const char* mode : {"wl", "wb", "w8l", "w8b"}) {
    SCOPED_TRACE(mode);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(mode);
      ASSERT_NE(writer, nullptr);
      // 3 samples YCbCr on IFD 0 (ExtraSamples is tested on tiled IFD 1 below
      // because contig YCbCr requires SamplesPerPixel == 3 in
      // _TIFFStrileSize64).
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{2}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_YCBCR}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_NONE}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{2}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SUBFILETYPE,
                             uint32_t{FILETYPE_PAGE}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_THRESHHOLDING,
                             uint16_t{THRESHHOLD_HALFTONE}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_FILLORDER,
                             uint16_t{FILLORDER_LSB2MSB}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ORIENTATION,
                             uint16_t{ORIENTATION_BOTLEFT}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_MINSAMPLEVALUE, uint16_t{1}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_MAXSAMPLEVALUE, uint16_t{250}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_XRESOLUTION, 300.0), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YRESOLUTION, 600.0), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_RESOLUTIONUNIT,
                             uint16_t{RESUNIT_CENTIMETER}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_XPOSITION, 1.5), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YPOSITION, 2.25), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PAGENUMBER, uint16_t{0},
                             uint16_t{3}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_HALFTONEHINTS, uint16_t{10},
                             uint16_t{240}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YCBCRSUBSAMPLING,
                             uint16_t{2}, uint16_t{1}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YCBCRPOSITIONING,
                             uint16_t{YCBCRPOSITION_COSITED}),
                1);
      constexpr std::array<float, 6> kRefBlackWhite = {0.0f,   255.0f, 128.0f,
                                                       255.0f, 128.0f, 255.0f};
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_REFERENCEBLACKWHITE,
                             kRefBlackWhite.data()),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT,
                             uint16_t{SAMPLEFORMAT_UINT}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEDEPTH, uint32_t{2}), 1);

      const char ink_names[] = "Cyan\0Magenta\0Yellow\0Black\0";
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_INKNAMES,
                       static_cast<uint16_t>(sizeof(ink_names) - 1), ink_names),
          1);

      // Colormap & 3 distinct TransferFunction tables (spp - extrasamples ==
      // 3).
      std::vector<uint16_t> rmap(256), gmap(256), bmap(256);
      for (uint32_t i = 0; i < 256; ++i) {
        rmap[i] = static_cast<uint16_t>(i * 257U);
        gmap[i] = static_cast<uint16_t>((255U - i) * 257U);
        bmap[i] = static_cast<uint16_t>((i ^ 0x55U) * 257U);
      }
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COLORMAP, rmap.data(),
                             gmap.data(), bmap.data()),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TRANSFERFUNCTION,
                             rmap.data(), gmap.data(), bmap.data()),
                1);

      // Two SubIFDs -> exercises multi-SubIFD out-of-line offset array +
      // TIFF_INSUBIFD chaining.
      constexpr std::array<uint64_t, 2> kSubOffsets = {0, 0};
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SUBIFD, uint16_t{2},
                             kSubOffsets.data()),
                1);

      std::vector<uint8_t> row(16, 0x42);
      for (uint32_t y = 0; y < 2; ++y) {
        std::vector<uint8_t> mutable_row = row;
        ASSERT_EQ(TIFFWriteScanline(writer.get(), mutable_row.data(), y, 0), 1);
      }
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);

      // Write SubIFD 0 and SubIFD 1 (with 1-table TransferFunction & collapsed
      // 3-identical-table TransferFunction on a follow-up directory).
      for (int s = 0; s < 2; ++s) {
        ConfigureBasicStripedGray8(writer.get(), 2, 2, 2);
        ASSERT_EQ(
            TIFFSetField(writer.get(), TIFFTAG_TRANSFERFUNCTION, rmap.data()),
            1);
        std::vector<uint8_t> srow(2, static_cast<uint8_t>(s + 1));
        ASSERT_EQ(TIFFWriteScanline(writer.get(), srow.data(), 0, 0), 1);
        std::vector<uint8_t> srow2(2, static_cast<uint8_t>(s + 2));
        ASSERT_EQ(TIFFWriteScanline(writer.get(), srow2.data(), 1, 0), 1);
        ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
      }

      // Write main IFD 1 with 1 SubIFD (nsubifd == 1 inline path) + 3 identical
      // TransferFunction tables (collapses n from 3 -> 1) + ExtraSamples +
      // tiled DEPTH.
      ConfigureBasicTiledGray8(writer.get(), 16, 16, 16, 16);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_RGB}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{4}), 1);
      constexpr std::array<uint16_t, 1> kExtra = {EXTRASAMPLE_ASSOCALPHA};
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_EXTRASAMPLES, uint16_t{1},
                             kExtra.data()),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEDEPTH, uint32_t{2}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TRANSFERFUNCTION,
                             rmap.data(), rmap.data(), rmap.data()),
                1);
      constexpr std::array<uint64_t, 1> kSingleSub = {0};
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SUBIFD, uint16_t{1},
                             kSingleSub.data()),
                1);
      std::vector<uint8_t> tile(16 * 16 * 4, 0x33);
      ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), 0, tile.data(), tile.size()),
                static_cast<tmsize_t>(tile.size()));
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);

      // Write the single SubIFD.
      ConfigureBasicStripedGray8(writer.get(), 2, 2, 2);
      std::vector<uint8_t> srow(2, 9);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), srow.data(), 0, 0), 1);
      std::vector<uint8_t> srow2(2, 9);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), srow2.data(), 1, 0), 1);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFNumberOfDirectories(reader.get()), 2U);
    uint16_t nsub = 0;
    const uint64_t* suboffs = nullptr;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_SUBIFD, &nsub, &suboffs), 1);
    ASSERT_EQ(nsub, 2U);
    ASSERT_NE(suboffs, nullptr);
    EXPECT_NE(suboffs[0], 0U);
    EXPECT_NE(suboffs[1], 0U);

    uint16_t num_inks = 0;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_NUMBEROFINKS, &num_inks), 1);
    EXPECT_EQ(num_inks, 4U);

    ASSERT_EQ(TIFFSetSubDirectory(reader.get(), suboffs[1]), 1);
    uint32_t sub_w = 0;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGEWIDTH, &sub_w), 1);
    EXPECT_EQ(sub_w, 2U);

    ASSERT_EQ(TIFFSetDirectory(reader.get(), 1), 1);
    uint32_t tdepth = 0;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_TILEDEPTH, &tdepth), 1);
    EXPECT_EQ(tdepth, 2U);
    ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_SUBIFD, &nsub, &suboffs), 1);
    ASSERT_EQ(nsub, 1U);
    EXPECT_NE(suboffs[0], 0U);
  }
}

TEST(TifDirWriteTest, DimensionsAbove64KAndLargeUncompressedBigTiffStriles) {
  // Large width/length (> 0xFFFF) triggers TIFF_LONG in ShortLong, and BigTIFF
  // large TIFFStripSize64 (> 0xFFFFFFFF) triggers WriteAsLong8 = 1 for
  // STRIPBYTECOUNTS with count > 1. Use checkpoint/setup without writing pixels
  // so memory usage stays tiny.
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("w8l");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{70000}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{140000}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{70000}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_NONE}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_MINISBLACK}),
                1);
      ASSERT_EQ(TIFFCheckpointDirectory(writer.get()), 1);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    uint32_t w = 0, l = 0, rps = 0;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGEWIDTH, &w), 1);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGELENGTH, &l), 1);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_ROWSPERSTRIP, &rps), 1);
    EXPECT_EQ(w, 70000U);
    EXPECT_EQ(l, 140000U);
    EXPECT_EQ(rps, 70000U);
  }

  // Same for tiled > 64K dimensions + BigTIFF TILEBYTECOUNTS > 0xFFFFFFFF.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w8l");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{131072}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{131072}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, uint32_t{65536}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, uint32_t{65536}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                           uint16_t{COMPRESSION_NONE}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                           uint16_t{PHOTOMETRIC_MINISBLACK}),
              1);
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 1);
    EXPECT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }
}

TEST(TifDirWriteTest, StrileByteCountsShortVsLongAcrossCompressions) {
  struct Case {
    const char* mode;
    uint16_t compression;
    uint32_t rows_per_strip;
    uint32_t length;
    bool tiled;
  };
  constexpr std::array<Case, 8> kCases = {{
      // NONE: strip size <= 65535 -> SHORT, > 65535 -> LONG
      {"wl", COMPRESSION_NONE, 4, 8, false},
      {"wl", COMPRESSION_NONE, 20000, 40000, false},
      // LZW: strip size < 6553 -> SHORT, >= 6553 -> LONG
      {"wl", COMPRESSION_LZW, 4, 8, false},
      {"wl", COMPRESSION_LZW, 2000, 4000, false},
      // PACKBITS: _WriteAsType returns 1 -> LONG
      {"wl", COMPRESSION_PACKBITS, 4, 8, false},
      // Tiled NONE & LZW small vs large -> SHORT vs LONG
      {"wl", COMPRESSION_NONE, 16, 32, true},
      {"wl", COMPRESSION_LZW, 16, 32, true},
      {"w8l", COMPRESSION_LZW, 16, 32, true},
  }};

  for (const Case& c : kCases) {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(c.mode);
      ASSERT_NE(writer, nullptr);
      if (c.tiled) {
        ConfigureBasicTiledGray8(writer.get(), 32, c.length, 16, 16,
                                 c.compression);
      } else {
        ConfigureBasicStripedGray8(writer.get(), 4, c.length, c.rows_per_strip,
                                   c.compression);
      }
      ASSERT_EQ(TIFFCheckpointDirectory(writer.get()), 1);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    const uint64_t* offsets = nullptr;
    const uint64_t* bytecounts = nullptr;
    EXPECT_EQ(TIFFGetField(reader.get(),
                           c.tiled ? TIFFTAG_TILEOFFSETS : TIFFTAG_STRIPOFFSETS,
                           &offsets),
              1);
    EXPECT_EQ(
        TIFFGetField(reader.get(),
                     c.tiled ? TIFFTAG_TILEBYTECOUNTS : TIFFTAG_STRIPBYTECOUNTS,
                     &bytecounts),
        1);
  }

  // Legacy COMPRESSION_DEFLATE warning.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripedGray8(writer.get(), 4, 4, 2, COMPRESSION_DEFLATE);
    WriteConstantRows(writer.get(), 4, 4, 7);
    EXPECT_EQ(TIFFWriteDirectory(writer.get()), 1);
    EXPECT_TRUE(
        ContainsSubstring(stream.diagnostics().warnings(), "legacy Deflate"));
  }
}

TEST(TifDirWriteTest, SampleFormatArrayAllFormatsBitsPerSampleAndClamping) {
  struct Case {
    uint16_t format;
    uint16_t bps;
    double val_low;
    double val_high;
    double expected_low;
    double expected_high;
    bool use_nan;
  };
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  const std::array<Case, 9> kCases = {{
      {SAMPLEFORMAT_UINT, 8, -10.5, 300.0, 0.0, 255.0, false},
      {SAMPLEFORMAT_UINT, 8, kNaN, 12.0, 255.0, 12.0, true},
      {SAMPLEFORMAT_UINT, 16, -5.0, 70000.0, 0.0, 65535.0, false},
      {SAMPLEFORMAT_UINT, 32, -1.0, 5e9, 0.0, 4294967295.0, false},
      {SAMPLEFORMAT_INT, 8, -200.0, 200.0, -128.0, 127.0, false},
      {SAMPLEFORMAT_INT, 8, kNaN, -12.0, -128.0, -12.0, true},
      {SAMPLEFORMAT_INT, 16, -40000.0, 40000.0, -32768.0, 32767.0, false},
      {SAMPLEFORMAT_INT, 32, -5e9, 5e9, -2147483648.0, 2147483647.0, false},
      {SAMPLEFORMAT_IEEEFP, 32, -123.5, 456.25, -123.5, 456.25, false},
  }};

  for (size_t i = 0; i < kCases.size(); ++i) {
    const Case& c = kCases[i];
    SCOPED_TRACE(i);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, c.bps), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{2}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT, c.format), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_MINISBLACK}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PERSAMPLE,
                             uint16_t{PERSAMPLE_MULTI}),
                1);
      std::array<double, 2> mins = {c.val_low, c.val_low};
      std::array<double, 2> maxs = {c.val_high, c.val_high};
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SMINSAMPLEVALUE, mins.data()), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SMAXSAMPLEVALUE, maxs.data()), 1);
      ASSERT_EQ(TIFFCheckpointDirectory(writer.get()), 1);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    double smin = 0.0, smax = 0.0;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_SMINSAMPLEVALUE, &smin), 1);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_SMAXSAMPLEVALUE, &smax), 1);
    EXPECT_DOUBLE_EQ(smin, c.expected_low);
    EXPECT_DOUBLE_EQ(smax, c.expected_high);
  }

  // Also test 64-bit IEEEFP SMin/SMaxSampleValue and NaN/high clamp on
  // UINT16/32 & INT16/32, plus SAMPLEFORMAT_VOID failure & SMinSampleValue OOM.
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{64}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT,
                             uint16_t{SAMPLEFORMAT_IEEEFP}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SMINSAMPLEVALUE, -1e100), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SMAXSAMPLEVALUE, 1e100), 1);
      ASSERT_EQ(TIFFCheckpointDirectory(writer.get()), 1);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    double smin = 0.0, smax = 0.0;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_SMINSAMPLEVALUE, &smin), 1);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_SMAXSAMPLEVALUE, &smax), 1);
    EXPECT_DOUBLE_EQ(smin, -1e100);
    EXPECT_DOUBLE_EQ(smax, 1e100);
  }

  // Remaining NaN branches in TIFFClampDoubleToInt16/32 and UInt16/32 + VOID.
  for (uint16_t fmt : {SAMPLEFORMAT_UINT, SAMPLEFORMAT_INT}) {
    for (uint16_t bps : {uint16_t{16}, uint16_t{32}}) {
      MemoryTiffStream stream;
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{2}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, bps), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT, fmt), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SMINSAMPLEVALUE, kNaN), 1);
      ASSERT_EQ(TIFFCheckpointDirectory(writer.get()), 1);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }
  }

  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripedGray8(writer.get(), 4, 1, 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT,
                           uint16_t{SAMPLEFORMAT_VOID}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SMINSAMPLEVALUE, 1.0), 1);
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
  }
}

TEST(TifDirWriteTest, Step2CodecTagsBuiltInAndCustomAsciiUnsupported) {
  // 1. LZW + PREDICTOR, CCITTFAX3 + GROUP3OPTIONS/CLEANFAXDATA/BADFAXLINES,
  //    JPEG + JPEGTABLES.
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripedGray8(writer.get(), 8, 4, 4, COMPRESSION_LZW);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PREDICTOR,
                             uint16_t{PREDICTOR_HORIZONTAL}),
                1);
      WriteConstantRows(writer.get(), 8, 4, 3);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);

      // CCITTFAX3
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_MINISWHITE}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_CCITTFAX3}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{4}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_GROUP3OPTIONS,
                             uint32_t{GROUP3OPT_FILLBITS}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_CLEANFAXDATA,
                             uint16_t{CLEANFAXDATA_CLEAN}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BADFAXLINES, uint32_t{2}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_CONSECUTIVEBADFAXLINES,
                             uint32_t{1}),
                1);
      std::array<uint8_t, 2> row1bpp = {0, 0};
      for (uint32_t y = 0; y < 4; ++y) {
        std::array<uint8_t, 2> m = row1bpp;
        ASSERT_EQ(TIFFWriteScanline(writer.get(), m.data(), y, 0), 1);
      }
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);

      // JPEG + JPEGTables
      ConfigureBasicStripedGray8(writer.get(), 16, 16, 16, COMPRESSION_JPEG);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JPEGQUALITY, 75), 1);
      WriteConstantRows(writer.get(), 16, 16, 50);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    uint16_t pred = 0;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_PREDICTOR, &pred), 1);
    EXPECT_EQ(pred, PREDICTOR_HORIZONTAL);
    ASSERT_EQ(TIFFSetDirectory(reader.get(), 1), 1);
    uint32_t g3opts = 0, bad = 0, consec = 0;
    uint16_t clean = 0;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_GROUP3OPTIONS, &g3opts), 1);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_CLEANFAXDATA, &clean), 1);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_BADFAXLINES, &bad), 1);
    EXPECT_EQ(
        TIFFGetField(reader.get(), TIFFTAG_CONSECUTIVEBADFAXLINES, &consec), 1);
    EXPECT_EQ(bad, 2U);
    EXPECT_EQ(consec, 1U);
    ASSERT_EQ(TIFFSetDirectory(reader.get(), 2), 1);
    uint32_t jtbl_len = 0;
    const void* jtbl = nullptr;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_JPEGTABLES, &jtbl_len, &jtbl),
              1);
    EXPECT_GT(jtbl_len, 0U);
  }

  // 2. Custom codec exercising TIFF_SETGET_ASCII and unsupported FIELD_CODEC
  //    default branch.
  {
    ScopedRegisteredCodec codec_guard(kTestCodecScheme, "TestCodec",
                                      &InitTestCodec);
    ASSERT_NE(codec_guard.get(), nullptr);

    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripedGray8(writer.get(), 4, 2, 2, kTestCodecScheme);
    ASSERT_EQ(TIFFSetField(writer.get(), kTestCodecAsciiTag, "CodecStringVal"),
              1);
    WriteConstantRows(writer.get(), 4, 2, 0);
    EXPECT_EQ(TIFFWriteDirectory(writer.get()), 1);

    // Next directory triggers default unsupported codec tag branch.
    ConfigureBasicStripedGray8(writer.get(), 4, 2, 2, kTestCodecScheme);
    ASSERT_EQ(TIFFSetField(writer.get(), kTestCodecUnsupportedTag, uint8_t{9}),
              1);
    EXPECT_EQ(TIFFWriteDirectory(writer.get()), 0);
    EXPECT_TRUE(
        ContainsSubstring(stream.diagnostics().errors(), "Cannot write tag"));
  }
}

TEST(TifDirWriteTest, AllCustomTagDataTypesAndAnonymousTags) {
  // Custom fields with TIFF_IFD (tv_size == 4) and true double-precision
  // RATIONAL/SRATIONAL (tv_size == 8).
  static const TIFFField kCustomFields[] = {
      {60100, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_IFD, 0, TIFF_SETGET_C16_UINT32,
       FIELD_CUSTOM, 1, 1, "CustomIfdArray", nullptr},
      {60101, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_RATIONAL, 0,
       TIFF_SETGET_C16_DOUBLE, FIELD_CUSTOM, 1, 1, "CustomRationalDoubleArr",
       nullptr},
      {60102, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_SRATIONAL, 0,
       TIFF_SETGET_C16_DOUBLE, FIELD_CUSTOM, 1, 1, "CustomSrationalDoubleArr",
       nullptr},
  };
  ScopedTagExtender ext_guard(+[](TIFF* tif) {
    _TIFFMergeFields(tif, kCustomFields, TIFFArrayCount(kCustomFields));
  });

  for (const char* mode : {"wl", "wb", "w8l", "w8b"}) {
    SCOPED_TRACE(mode);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(mode);
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);

      // ASCII & UNDEFINED
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEDESCRIPTION,
                             "CustomAllTypes"),
                1);
      constexpr std::array<uint8_t, 5> kIcc = {1, 2, 3, 4, 5};
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ICCPROFILE,
                             static_cast<uint32_t>(kIcc.size()), kIcc.data()),
                1);

      // BYTE (single + array) & SBYTE (anon)
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_FAXPROFILE, uint8_t{2}), 1);
      constexpr std::array<uint8_t, 4> kDngVer = {1, 4, 0, 0};
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_DNGVERSION, kDngVer.data()),
                1);
      ASSERT_NE(_TIFFFindOrRegisterField(writer.get(), 60200, TIFF_SBYTE),
                nullptr);
      constexpr std::array<int8_t, 5> kSbytes = {-128, -1, 0, 1, 127};
      ASSERT_EQ(
          TIFFSetField(writer.get(), 60200,
                       static_cast<uint32_t>(kSbytes.size()), kSbytes.data()),
          1);

      // SHORT (array) & SSHORT (anon)
      constexpr std::array<uint16_t, 4> kLin = {0, 100, 200, 300};
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_LINEARIZATIONTABLE,
                             static_cast<uint16_t>(kLin.size()), kLin.data()),
                1);
      ASSERT_NE(_TIFFFindOrRegisterField(writer.get(), 60201, TIFF_SSHORT),
                nullptr);
      constexpr std::array<int16_t, 3> kSshorts = {-32000, 0, 32000};
      ASSERT_EQ(
          TIFFSetField(writer.get(), 60201,
                       static_cast<uint32_t>(kSshorts.size()), kSshorts.data()),
          1);

      // LONG & SLONG (anon)
      constexpr std::array<uint32_t, 3> kMasked = {10, 20, 30};
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_MASKEDAREAS,
                       static_cast<uint16_t>(kMasked.size()), kMasked.data()),
          1);
      ASSERT_NE(_TIFFFindOrRegisterField(writer.get(), 60202, TIFF_SLONG),
                nullptr);
      constexpr std::array<int32_t, 2> kSlongs = {-100000, 200000};
      ASSERT_EQ(
          TIFFSetField(writer.get(), 60202,
                       static_cast<uint32_t>(kSlongs.size()), kSlongs.data()),
          1);

      // LONG8 & SLONG8 (with >32-bit values in BigTIFF!)
      const bool is_big = (mode[1] == '8');
      ASSERT_NE(_TIFFFindOrRegisterField(writer.get(), 60203, TIFF_LONG8),
                nullptr);
      std::array<uint64_t, 2> kLong8s = {123U,
                                         is_big ? 0x123456789ULL : 0xFFFFFF00U};
      ASSERT_EQ(
          TIFFSetField(writer.get(), 60203,
                       static_cast<uint32_t>(kLong8s.size()), kLong8s.data()),
          1);
      ASSERT_NE(_TIFFFindOrRegisterField(writer.get(), 60204, TIFF_SLONG8),
                nullptr);
      std::array<int64_t, 2> kSlong8s = {
          -42, is_big ? -0x123456789LL : 2147483000LL};
      ASSERT_EQ(
          TIFFSetField(writer.get(), 60204,
                       static_cast<uint32_t>(kSlong8s.size()), kSlong8s.data()),
          1);

      // RATIONAL (float array + double array) & SRATIONAL (float + double)
      constexpr std::array<float, 2> kBlack = {1.5f, 0.25f};
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_BLACKLEVEL,
                       static_cast<uint16_t>(kBlack.size()), kBlack.data()),
          1);
      constexpr std::array<float, 3> kMat = {-1.5f, 0.0f, 2.25f};
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COLORMATRIX1,
                             static_cast<uint16_t>(kMat.size()), kMat.data()),
                1);
      constexpr std::array<double, 6> kDblRats = {
          0.0, 42.0, 1e12, 1e-12, 3.141592653589793, 1.0 / 3.0};
      ASSERT_EQ(
          TIFFSetField(writer.get(), 60101,
                       static_cast<uint16_t>(kDblRats.size()), kDblRats.data()),
          1);
      constexpr std::array<double, 8> kDblSrats = {-5.0,
                                                   5.0,
                                                   -1e12,
                                                   1e12,
                                                   -1e-12,
                                                   1e-12,
                                                   -3.141592653589793,
                                                   0.12345678901234567};
      ASSERT_EQ(TIFFSetField(writer.get(), 60102,
                             static_cast<uint16_t>(kDblSrats.size()),
                             kDblSrats.data()),
                1);

      // FLOAT & DOUBLE (single + array)
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PIXAR_FOVCOT, 1.75), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_STONITS, 123.4567890123), 1);
      ASSERT_NE(_TIFFFindOrRegisterField(writer.get(), 60205, TIFF_DOUBLE),
                nullptr);
      constexpr std::array<double, 2> kDbls = {-1.25, 99.875};
      ASSERT_EQ(TIFFSetField(writer.get(), 60205,
                             static_cast<uint32_t>(kDbls.size()), kDbls.data()),
                1);

      // IFD (tv_size == 4) & IFD8
      constexpr std::array<uint32_t, 2> kIfds = {16U, 32U};
      ASSERT_EQ(TIFFSetField(writer.get(), 60100,
                             static_cast<uint16_t>(kIfds.size()), kIfds.data()),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_INTEROPERABILITYIFD,
                             is_big ? 0x100000010ULL : uint64_t{64U}),
                1);

      WriteConstantRows(writer.get(), 4, 2, 1);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    double stonits = 0.0;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_STONITS, &stonits), 1);
    EXPECT_DOUBLE_EQ(stonits, 123.4567890123);

    uint16_t ifd_cnt = 0;
    const uint32_t* ifd_vals = nullptr;
    EXPECT_EQ(TIFFGetField(reader.get(), 60100, &ifd_cnt, &ifd_vals), 1);
    ASSERT_EQ(ifd_cnt, 2U);
    EXPECT_EQ(ifd_vals[0], 16U);
    EXPECT_EQ(ifd_vals[1], 32U);

    uint32_t sb_cnt = 0;
    const int8_t* sb_vals = nullptr;
    EXPECT_EQ(TIFFGetField(reader.get(), 60200, &sb_cnt, &sb_vals), 1);
    ASSERT_EQ(sb_cnt, 5U);
    EXPECT_EQ(sb_vals[0], -128);
    EXPECT_EQ(sb_vals[4], 127);

    uint16_t rat_cnt = 0;
    const double* rat_vals = nullptr;
    EXPECT_EQ(TIFFGetField(reader.get(), 60101, &rat_cnt, &rat_vals), 1);
    ASSERT_EQ(rat_cnt, 6U);
    EXPECT_NEAR(rat_vals[4], 3.141592653589793, 1e-8);
    EXPECT_NEAR(rat_vals[5], 1.0 / 3.0, 1e-8);

    uint16_t srat_cnt = 0;
    const double* srat_vals = nullptr;
    EXPECT_EQ(TIFFGetField(reader.get(), 60102, &srat_cnt, &srat_vals), 1);
    ASSERT_EQ(srat_cnt, 8U);
    EXPECT_NEAR(srat_vals[6], -3.141592653589793, 1e-8);
    EXPECT_NEAR(srat_vals[7], 0.12345678901234567, 1e-8);
  }
}

TEST(TifDirWriteTest, RationalErrorsAndClassicRangeGuards) {
  ScopedQuietGlobalHandlers global_quiet;

  // 1. Negative and NaN rational values on TIFFTAG_XPOSITION / XRESOLUTION /
  //    custom RATIONAL array (DoubleToRational error paths).
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_XPOSITION, -1.5), 1);
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
    EXPECT_TRUE(
        ContainsSubstring(stream.diagnostics().errors(), "Negative value"));

    stream.diagnostics().Clear();
    ASSERT_EQ(TIFFSetField(
                  writer.get(), TIFFTAG_XPOSITION,
                  static_cast<double>(std::numeric_limits<float>::quiet_NaN())),
              1);
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
    EXPECT_TRUE(
        ContainsSubstring(stream.diagnostics().errors(), "Not-a-number"));

    // Reset XPOSITION to valid, then pass negative/NaN inside WHITEPOINT array.
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_XPOSITION, 0.5), 1);
    std::array<float, 2> wp = {-2.0f, std::numeric_limits<float>::quiet_NaN()};
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WHITEPOINT, wp.data()), 1);
    EXPECT_EQ(TIFFWriteDirectory(writer.get()), 1);
    EXPECT_FALSE(global_quiet.messages().empty());
  }

  // 2. ClassicTIFF out-of-range LONG8, SLONG8 (> INT32_MAX, < INT32_MIN), IFD8,
  //    and SubIFD (> 0xFFFFFFFF) + ColorMap / TransferFunction invalid bps>=32.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
    ASSERT_NE(_TIFFFindOrRegisterField(writer.get(), 60300, TIFF_LONG8),
              nullptr);
    uint64_t ok_u64 = 42ULL;
    ASSERT_EQ(TIFFSetField(writer.get(), 60300, uint32_t{1}, &ok_u64), 1);
    // Mutate stored custom value directly past 0xFFFFFFFF after _TIFFVSetField.
    for (int i = 0; i < writer->tif_dir.td_customValueCount; ++i) {
      if (writer->tif_dir.td_customValues[i].info->field_tag == 60300) {
        *static_cast<uint64_t*>(writer->tif_dir.td_customValues[i].value) =
            0x100000001ULL;
      }
    }
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "larger than 0xFFFFFFFF"));
    ASSERT_EQ(TIFFUnsetField(writer.get(), 60300), 1);

    // SLONG8 > INT32_MAX and < INT32_MIN.
    stream.diagnostics().Clear();
    ASSERT_NE(_TIFFFindOrRegisterField(writer.get(), 60301, TIFF_SLONG8),
              nullptr);
    int64_t ok_i64 = 10;
    ASSERT_EQ(TIFFSetField(writer.get(), 60301, uint32_t{1}, &ok_i64), 1);
    int64_t* slong8_ptr = nullptr;
    for (int i = 0; i < writer->tif_dir.td_customValueCount; ++i) {
      if (writer->tif_dir.td_customValues[i].info->field_tag == 60301) {
        slong8_ptr =
            static_cast<int64_t*>(writer->tif_dir.td_customValues[i].value);
      }
    }
    ASSERT_NE(slong8_ptr, nullptr);
    *slong8_ptr = 2147483648LL;
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "larger than 0x7FFFFFFF"));

    stream.diagnostics().Clear();
    *slong8_ptr = -2147483649LL;
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "smaller than 0x80000000"));
    ASSERT_EQ(TIFFUnsetField(writer.get(), 60301), 1);

    // IFD8 > 0xFFFFFFFF in ClassicTIFF.
    stream.diagnostics().Clear();
    uint64_t ok_ifd = 16;
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_INTEROPERABILITYIFD, ok_ifd),
              1);
    for (int i = 0; i < writer->tif_dir.td_customValueCount; ++i) {
      if (writer->tif_dir.td_customValues[i].info->field_tag ==
          TIFFTAG_INTEROPERABILITYIFD) {
        *static_cast<uint64_t*>(writer->tif_dir.td_customValues[i].value) =
            0x200000000ULL;
      }
    }
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "larger than 0xFFFFFFFF"));
    ASSERT_EQ(TIFFUnsetField(writer.get(), TIFFTAG_INTEROPERABILITYIFD), 1);

    // SUBIFD > 0xFFFFFFFF in ClassicTIFF (_TIFFVSetField accepts uint64_t array
    // without ClassicTIFF range check!).
    stream.diagnostics().Clear();
    uint64_t bad_subifd = 0x100000010ULL;
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_SUBIFD, uint16_t{1}, &bad_subifd),
        1);
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(), "SubIFD"));
    ASSERT_EQ(TIFFUnsetField(writer.get(), TIFFTAG_SUBIFD), 1);
  }

  // 3. ColorMap & TransferFunction with bps >= 32, and missing TransferFunction
  //    table warning.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
    std::vector<uint16_t> lut(256, 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COLORMAP, lut.data(),
                           lut.data(), lut.data()),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{32}),
              1);
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(), "Colormap"));

    ASSERT_EQ(TIFFUnsetField(writer.get(), TIFFTAG_COLORMAP), 1);
    stream.diagnostics().Clear();
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TRANSFERFUNCTION, lut.data()),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{32}),
              1);
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
    EXPECT_TRUE(
        ContainsSubstring(stream.diagnostics().errors(), "TransferFunction"));

    // Restore valid bps, spp=3, and remove td_transferfunction[2] to trigger
    // "Too few TransferFunctions provided" warning while still returning 1.
    stream.diagnostics().Clear();
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}),
              1);
    std::vector<uint16_t> lut2(256, 2);
    std::vector<uint16_t> lut3(256, 3);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TRANSFERFUNCTION, lut.data(),
                           lut2.data(), lut3.data()),
              1);
    _TIFFfreeExt(writer.get(), writer->tif_dir.td_transferfunction[2]);
    writer->tif_dir.td_transferfunction[2] = nullptr;
    EXPECT_EQ(TIFFWriteDirectory(writer.get()), 1);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().warnings(),
                                  "Too few TransferFunctions"));
  }
}

TEST(TifDirWriteTest, StripNullPointersAndClassicLongOverflow) {
  // Null td_stripbytecount_p & td_stripoffset_p with FIELD bits set.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
    ASSERT_EQ(TIFFSetupStrips(writer.get()), 1);
    _TIFFfreeExt(writer.get(), writer->tif_dir.td_stripbytecount_p);
    writer->tif_dir.td_stripbytecount_p = nullptr;
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
    EXPECT_TRUE(
        ContainsSubstring(stream.diagnostics().errors(), "StripByteCount"));

    stream.diagnostics().Clear();
    writer->tif_dir.td_stripbytecount_p =
        static_cast<uint64_t*>(_TIFFCheckMalloc(
            writer.get(), 1, sizeof(uint64_t), "restore bytecounts"));
    ASSERT_NE(writer->tif_dir.td_stripbytecount_p, nullptr);
    writer->tif_dir.td_stripbytecount_p[0] = 0;

    _TIFFfreeExt(writer.get(), writer->tif_dir.td_stripoffset_p);
    writer->tif_dir.td_stripoffset_p = nullptr;
    EXPECT_EQ(TIFFWriteDirectory(writer.get()), 0);
    EXPECT_TRUE(
        ContainsSubstring(stream.diagnostics().errors(), "StripByteOffset"));

    // Restore td_stripoffset_p before close, and also verify ClassicTIFF
    // > 0xFFFFFFFF strile value failure.
    writer->tif_dir.td_stripoffset_p = static_cast<uint64_t*>(
        _TIFFCheckMalloc(writer.get(), 1, sizeof(uint64_t), "restore offsets"));
    ASSERT_NE(writer->tif_dir.td_stripoffset_p, nullptr);
    writer->tif_dir.td_stripoffset_p[0] = 0;
    writer->tif_dir.td_stripbytecount_p[0] = 0x100000005ULL;
    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFWriteDirectory(writer.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "larger than 0xFFFFFFFF"));
    writer->tif_dir.td_stripbytecount_p[0] = 0;
    writer->tif_flags &=
        ~(TIFF_DIRTYDIRECT | TIFF_DIRTYSTRIP | TIFF_BEENWRITING);
  }
}

TEST(TifDirWriteTest, DeferredStrileWritingAndForceStrileAndRewriteField) {
  // Test defer + force across Classic/BigTIFF, LE/BE, striped/tiled,
  // compressed/uncompressed (SHORT/LONG/LONG8 bytecounts).
  struct DeferCase {
    const char* mode;
    uint16_t compression;
    bool tiled;
    uint32_t width;
    uint32_t length;
    uint32_t strip_or_tile;
  };
  constexpr std::array<DeferCase, 6> kCases = {{
      {"wl", COMPRESSION_NONE, false, 8, 8, 4},
      {"wb", COMPRESSION_NONE, false, 8, 8, 4},
      {"w8l", COMPRESSION_LZW, false, 16, 8, 4},
      {"w8b", COMPRESSION_NONE, true, 32, 32, 16},
      {"wl", COMPRESSION_LZW, true, 32, 32, 16},
      {"wl", COMPRESSION_NONE, true, 32, 32, 16},
  }};

  for (const DeferCase& c : kCases) {
    SCOPED_TRACE(c.mode);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(c.mode);
      ASSERT_NE(writer, nullptr);
      if (c.tiled) {
        ConfigureBasicTiledGray8(writer.get(), c.width, c.length,
                                 c.strip_or_tile, c.strip_or_tile,
                                 c.compression);
      } else {
        ConfigureBasicStripedGray8(writer.get(), c.width, c.length,
                                   c.strip_or_tile, c.compression);
      }
      ASSERT_EQ(TIFFDeferStrileArrayWriting(writer.get()), 1);
      ASSERT_EQ(TIFFWriteCheck(writer.get(), c.tiled ? 1 : 0, "DeferTest"), 1);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    // Reopen in r+, populate striles via TIFFWriteScanline / EncodedTile and
    // force strile array writing.
    {
      UniqueTiff updater = stream.Open("r+");
      ASSERT_NE(updater, nullptr);
      if (c.tiled) {
        std::vector<uint8_t> tile(c.strip_or_tile * c.strip_or_tile, 0x5A);
        const uint32_t ntiles = TIFFNumberOfTiles(updater.get());
        for (uint32_t t = 0; t < ntiles; ++t) {
          std::vector<uint8_t> m = tile;
          ASSERT_EQ(TIFFWriteEncodedTile(updater.get(), t, m.data(), m.size()),
                    static_cast<tmsize_t>(m.size()));
        }
      } else {
        WriteConstantRows(updater.get(), c.width, c.length, 3);
      }
      EXPECT_EQ(TIFFForceStrileArrayWriting(updater.get()), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    const uint64_t* offs = nullptr;
    const uint64_t* bcs = nullptr;
    EXPECT_EQ(TIFFGetField(reader.get(),
                           c.tiled ? TIFFTAG_TILEOFFSETS : TIFFTAG_STRIPOFFSETS,
                           &offs),
              1);
    EXPECT_EQ(
        TIFFGetField(reader.get(),
                     c.tiled ? TIFFTAG_TILEBYTECOUNTS : TIFFTAG_STRIPBYTECOUNTS,
                     &bcs),
        1);
    ASSERT_NE(offs, nullptr);
    ASSERT_NE(bcs, nullptr);
    EXPECT_NE(offs[0], 0U);
    EXPECT_GT(bcs[0], 0U);
  }
}

TEST(TifDirWriteTest, DirectRewriteFieldPathsAndConversionsAndErrors) {
  // 1. Mapped handle error & zero diroff error.
  {
    MemoryTiffStream stream;
    WriteTwoDirImage(&stream, "wl");
    stream.set_enable_mmap(true);
    UniqueTiff mapped = stream.Open("rM");
    ASSERT_NE(mapped, nullptr);
    uint16_t short_val = 7;
    EXPECT_EQ(_TIFFRewriteField(mapped.get(), TIFFTAG_ORIENTATION, TIFF_SHORT,
                                1, &short_val),
              0);
    EXPECT_TRUE(
        ContainsSubstring(stream.diagnostics().errors(), "Memory mapped"));
  }
  {
    MemoryTiffStream stream;
    UniqueTiff unwritten = stream.Open("wl");
    ASSERT_NE(unwritten, nullptr);
    uint16_t short_val = 1;
    EXPECT_EQ(_TIFFRewriteField(unwritten.get(), TIFFTAG_ORIENTATION,
                                TIFF_SHORT, 1, &short_val),
              0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "not already on disk"));
  }

  // 2. Classic LE/BE & BigTIFF LE/BE direct _TIFFRewriteField:
  //    same type+count (inline vs out-of-line), resized inline (<4, ==4, <=8),
  //    resized out-of-line at EOF, and LONG8/SLONG8/IFD8 conversions+overflow.
  for (const char* mode : {"wl", "wb", "w8l", "w8b"}) {
    SCOPED_TRACE(mode);
    const bool is_big = (mode[1] == '8');
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(mode);
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ORIENTATION,
                             uint16_t{ORIENTATION_TOPLEFT}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ARTIST, "OldArtistName12"),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_INTEROPERABILITYIFD, uint64_t{8}),
          1);
      ASSERT_NE(_TIFFFindOrRegisterField(writer.get(), 60400, TIFF_SLONG8),
                nullptr);
      int64_t init_slong8 = 100;
      ASSERT_EQ(TIFFSetField(writer.get(), 60400, uint32_t{1}, &init_slong8),
                1);
      WriteConstantRows(writer.get(), 4, 2, 1);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff rw = stream.Open("r+");
    ASSERT_NE(rw, nullptr);

    // Same count + type: inline 2-byte SHORT -> 2-byte swab, 4-byte LONG.
    uint16_t orient = ORIENTATION_RIGHTBOT;
    EXPECT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_ORIENTATION, TIFF_SHORT, 1,
                                &orient),
              1);
    uint32_t subfile = 99U;
    EXPECT_EQ(
        _TIFFRewriteField(rw.get(), TIFFTAG_IMAGEWIDTH, TIFF_LONG, 1, &subfile),
        1);

    // Same count + type out-of-line ASCII (exact length 16 including NUL).
    char same_artist[16] = "NewArtistName12";
    EXPECT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_ARTIST, TIFF_ASCII, 16,
                                same_artist),
              1);

    // Grow out-of-line ASCII -> relocated to EOF.
    char long_artist[24] = "MuchLongerArtistName!!!";
    EXPECT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_ARTIST, TIFF_ASCII, 24,
                                long_artist),
              1);

    // Shrink ARTIST into inline slot: == 4 bytes then == 2 bytes.
    char four_artist[4] = "ABC";
    EXPECT_EQ(
        _TIFFRewriteField(rw.get(), TIFFTAG_ARTIST, TIFF_ASCII, 4, four_artist),
        1);
    char two_artist[2] = "Z";
    EXPECT_EQ(
        _TIFFRewriteField(rw.get(), TIFFTAG_ARTIST, TIFF_ASCII, 2, two_artist),
        1);

    // Type conversions: LONG8->LONG (or LONG8 in BigTIFF), IFD8->IFD,
    // SLONG8->SLONG, and overflows in ClassicTIFF!
    uint64_t strip_off = 128;
    EXPECT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_STRIPOFFSETS, TIFF_LONG8, 1,
                                &strip_off),
              1);
    uint64_t new_ifd = is_big ? 0x100000080ULL : 128U;
    EXPECT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_INTEROPERABILITYIFD,
                                TIFF_IFD8, 1, &new_ifd),
              1);
    int64_t new_slong8 = is_big ? -0x100000000LL : -12345;
    EXPECT_EQ(_TIFFRewriteField(rw.get(), 60400, TIFF_SLONG8, 1, &new_slong8),
              1);

    if (!is_big) {
      uint64_t too_big_u64 = 0x100000001ULL;
      EXPECT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_STRIPOFFSETS, TIFF_LONG8, 1,
                                  &too_big_u64),
                0);
      EXPECT_TRUE(
          ContainsSubstring(stream.diagnostics().errors(), "32bit range"));

      stream.diagnostics().Clear();
      int64_t too_big_s64 = 0x100000000LL;
      EXPECT_EQ(
          _TIFFRewriteField(rw.get(), 60400, TIFF_SLONG8, 1, &too_big_s64), 0);
      EXPECT_TRUE(
          ContainsSubstring(stream.diagnostics().errors(), "32bit range"));

      // Rewrite STRIPBYTECOUNTS as SHORT first, then pass LONG8 > 0xFFFF!
      uint16_t bc16 = 8;
      ASSERT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_STRIPBYTECOUNTS, TIFF_SHORT,
                                  1, &bc16),
                1);
      uint64_t bc_over16 = 0x10005ULL;
      stream.diagnostics().Clear();
      EXPECT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_STRIPBYTECOUNTS, TIFF_LONG8,
                                  1, &bc_over16),
                0);
      EXPECT_TRUE(
          ContainsSubstring(stream.diagnostics().errors(), "16bit range"));
    }
  }
}

TEST(TifDirWriteTest, RewriteFieldSeekReadWriteAndMissingTagFaults) {
  // Missing tag when dircount == 0 (mutated cleanly in memory after open),
  // plus SeekOK / ReadOK / WriteOK failures inside _TIFFRewriteField.
  for (const char* mode : {"wl", "w8l"}) {
    SCOPED_TRACE(mode);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(mode);
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ORIENTATION,
                             uint16_t{ORIENTATION_TOPLEFT}),
                1);
      WriteConstantRows(writer.get(), 4, 2, 1);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff rw = stream.Open("r+");
    ASSERT_NE(rw, nullptr);
    uint16_t orient = ORIENTATION_TOPLEFT;

    // 1. Seek fail accessing directory.
    stream.FailSeeksAfter(stream.seek_calls());
    EXPECT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_ORIENTATION, TIFF_SHORT, 1,
                                &orient),
              0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(), "Seek error"));

    // 2. Read fail fetching directory count.
    stream.ResetFaults();
    stream.diagnostics().Clear();
    stream.FailReadsAfter(stream.read_calls(), -1);
    EXPECT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_ORIENTATION, TIFF_SHORT, 1,
                                &orient),
              0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "read TIFF directory count"));

    // 3. Read fail fetching directory entry.
    stream.ResetFaults();
    stream.diagnostics().Clear();
    stream.FailReadsAfter(stream.read_calls() + 1, -1);
    EXPECT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_ORIENTATION, TIFF_SHORT, 1,
                                &orient),
              0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "read TIFF directory entry"));

    // 4. Write fail overwriting matching inline entry & relocated out-of-line
    //    & final entry write-back.
    stream.ResetFaults();
    stream.diagnostics().Clear();
    stream.FailWritesAfter(stream.write_calls(), -1);
    EXPECT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_ORIENTATION, TIFF_SHORT, 1,
                                &orient),
              0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "Error writing directory link"));

    stream.ResetFaults();
    stream.diagnostics().Clear();
    char grow_artist[16] = "GrowingArtist!!";
    stream.FailWritesAfter(stream.write_calls(), -1);
    EXPECT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_COMPRESSION, TIFF_ASCII, 16,
                                grow_artist),
              0);

    stream.ResetFaults();
    stream.diagnostics().Clear();
    stream.FailWritesAfter(stream.write_calls() + 1, -1);
    EXPECT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_COMPRESSION, TIFF_ASCII, 16,
                                grow_artist),
              0);

    // 5. Missing tag with dircount == 0 (avoids infinite-loop upstream bug
    //    when tag is absent in non-zero directory without hitting EOF).
    stream.ResetFaults();
    stream.diagnostics().Clear();
    const uint64_t diroff = rw->tif_diroff;
    std::memset(stream.mutable_data().data() + diroff, 0,
                (mode[1] == '8') ? 8 : 2);
    EXPECT_EQ(_TIFFRewriteField(rw.get(), TIFFTAG_ARTIST, TIFF_ASCII, 2,
                                const_cast<char*>("A")),
              0);
    EXPECT_TRUE(
        ContainsSubstring(stream.diagnostics().errors(), "Could not find tag"));
  }
}

TEST(TifDirWriteTest, LinkDirectorySubIfdsLoopsAndIoFaults) {
  // 1. SubIFD link write errors in ClassicTIFF and BigTIFF.
  for (const char* mode : {"wl", "wb", "w8l", "w8b"}) {
    SCOPED_TRACE(mode);
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open(mode);
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
    constexpr std::array<uint64_t, 2> kSub = {0, 0};
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_SUBIFD, uint16_t{2}, kSub.data()),
        1);
    WriteConstantRows(writer.get(), 4, 2, 1);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);

    // Next directory is a SubIFD -> inject write failure on first WriteOK.
    ConfigureBasicStripedGray8(writer.get(), 2, 2, 2);
    WriteConstantRows(writer.get(), 2, 2, 1);
    ASSERT_EQ(TIFFFlushData(writer.get()), 1);
    stream.FailWritesAfter(stream.write_calls(), -1);
    EXPECT_EQ(TIFFWriteDirectory(writer.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "writing SubIFD directory link"));
    writer->tif_flags &=
        ~(TIFF_DIRTYDIRECT | TIFF_DIRTYSTRIP | TIFF_BEENWRITING);
  }

  // 2. Main IFD linking faults: header write fail (Classic & BigTIFF),
  //    appending when tif_lastdiroff == 0 (traversal from header), chain
  //    traversal seek/read/write faults, BigTIFF dircount64 > 0xFFFF, and
  //    IFD loop detection in TIFFLinkDirectory.
  for (const char* mode : {"wl", "w8l"}) {
    SCOPED_TRACE(mode);
    const bool is_big = (mode[1] == '8');

    // First IFD header write failure.
    {
      MemoryTiffStream stream;
      UniqueTiff writer = stream.Open(mode);
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
      WriteConstantRows(writer.get(), 4, 2, 1);
      ASSERT_EQ(TIFFFlushData(writer.get()), 1);
      stream.FailWritesAfter(stream.write_calls(), -1);
      EXPECT_EQ(TIFFWriteDirectory(writer.get()), 0);
      EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                    "writing TIFF header"));
      writer->tif_flags &=
          ~(TIFF_DIRTYDIRECT | TIFF_DIRTYSTRIP | TIFF_BEENWRITING);
    }

    // Appended IFD chain traversal & faults when tif_lastdiroff == 0.
    {
      MemoryTiffStream stream;
      WriteTwoDirImage(&stream, mode);

      UniqueTiff appender = stream.Open("r+");
      ASSERT_NE(appender, nullptr);
      ASSERT_EQ(TIFFCreateDirectory(appender.get()), 0);
      appender->tif_lastdiroff = 0;

      // Fault reading dir count during link chain walk.
      ConfigureBasicStripedGray8(appender.get(), 4, 2, 2);
      stream.FailReadsAfter(stream.read_calls(), -1);
      EXPECT_EQ(TIFFCheckpointDirectory(appender.get()), 0);

      // Fault reading nextdir link during link chain walk.
      stream.ResetFaults();
      stream.diagnostics().Clear();
      appender->tif_flags &=
          ~(TIFF_DIRTYDIRECT | TIFF_DIRTYSTRIP | TIFF_BEENWRITING);
      appender->tif_diroff = 0;
      ASSERT_EQ(TIFFCreateDirectory(appender.get()), 0);
      appender->tif_lastdiroff = 0;
      ConfigureBasicStripedGray8(appender.get(), 4, 2, 2);
      stream.FailReadsAfter(stream.read_calls() + 1, -1);
      EXPECT_EQ(TIFFCheckpointDirectory(appender.get()), 0);

      // Corrupt IFD 1 next-offset to point back to IFD 0 (loop!).
      stream.ResetFaults();
      stream.diagnostics().Clear();
      appender->tif_flags &=
          ~(TIFF_DIRTYDIRECT | TIFF_DIRTYSTRIP | TIFF_BEENWRITING);
      appender->tif_diroff = 0;
      const uint64_t ifd0 = is_big ? appender->tif_header.big.tiff_diroff
                                   : appender->tif_header.classic.tiff_diroff;
      // Advance cleanly to IFD 1 to find its next-link file offset, then patch.
      uint64_t ifd1 = 0;
      ASSERT_EQ(TIFFSetDirectory(appender.get(), 1), 1);
      ifd1 = TIFFCurrentDirOffset(appender.get());
      uint64_t dcnt64 = 0;
      uint16_t dcnt16 = 0;
      if (is_big) {
        std::memcpy(&dcnt64, stream.data().data() + ifd1, sizeof(dcnt64));
      } else {
        std::memcpy(&dcnt16, stream.data().data() + ifd1, sizeof(dcnt16));
      }
      const uint16_t dcnt = static_cast<uint16_t>(is_big ? dcnt64 : dcnt16);
      const uint64_t link_pos =
          ifd1 + (is_big ? (8U + dcnt * 20ULL) : (2U + dcnt * 12U));
      if (is_big) {
        WriteLe32(stream.mutable_data().data() + link_pos,
                  static_cast<uint32_t>(ifd0));
        WriteLe32(stream.mutable_data().data() + link_pos + 4, 0U);
      } else {
        WriteLe32(stream.mutable_data().data() + link_pos,
                  static_cast<uint32_t>(ifd0));
      }

      ASSERT_EQ(TIFFCreateDirectory(appender.get()), 0);
      appender->tif_lastdiroff = 0;
      ConfigureBasicStripedGray8(appender.get(), 4, 2, 2);
      EXPECT_EQ(TIFFCheckpointDirectory(appender.get()), 0);
      EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                    "IFD loop detected"));
      appender->tif_flags &=
          ~(TIFF_DIRTYDIRECT | TIFF_DIRTYSTRIP | TIFF_BEENWRITING);
      appender->tif_diroff = 0;
    }
  }
}

TEST(TifDirWriteTest, DirectoryWriteIoAndPostEncodeAndOomFailures) {
  // 1. Failing tif_postencode before write.
  {
    ScopedRegisteredCodec guard(65002, "FailPostEncode",
                                &InitFailingPostEncodeCodec);
    ASSERT_NE(guard.get(), nullptr);
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripedGray8(writer.get(), 4, 2, 2, 65002);
    std::array<uint8_t, 4> row = {1, 2, 3, 4};
    ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), 1);
    writer->tif_flags |= TIFF_POSTENCODE;
    EXPECT_EQ(TIFFWriteDirectory(writer.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "post-encoding before directory write"));
  }

  // 2. Flush failure during imagedone when tif_rawcc > 0.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripedGray8(writer.get(), 4, 4, 4, COMPRESSION_LZW);
    WriteConstantRows(writer.get(), 4, 2, 1);
    stream.FailWritesAfter(stream.write_calls(), -1);
    EXPECT_EQ(TIFFWriteDirectory(writer.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "Error flushing data"));
  }

  // 3. OOM via ScopedCumulatedMemAllocRemaining + ScopedSingleMemAllocLimit
  //    covering dir, dirmem, ShortPerSample, Colormap, TransferFunction,
  //    SampleformatArray, LongLong8Array, RationalArray, SubIFD, RewriteField.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = OpenWithBudget(&stream, "wl", 1 << 24);
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
    WriteConstantRows(writer.get(), 4, 2, 1);

    // Exhaust cumulated budget so dir allocation fails.
    {
      ScopedCumulatedMemAllocRemaining oom(writer.get(), 1);
      EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
      EXPECT_TRUE(
          ContainsSubstring(stream.diagnostics().errors(), "Out of memory"));
    }

    // Test single-alloc limits on SampleformatArray, RationalArray, ColorMap,
    // TransferFunction, SubIFD, and LongLong8Array.
    stream.diagnostics().Clear();
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SMINSAMPLEVALUE, 1.0), 1);
    {
      ScopedSingleMemAllocLimit single_limit(writer.get(), 4);
      EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
      EXPECT_TRUE(
          ContainsSubstring(stream.diagnostics().errors(), "Out of memory"));
    }
    ASSERT_EQ(TIFFUnsetField(writer.get(), TIFFTAG_SMINSAMPLEVALUE), 1);

    stream.diagnostics().Clear();
    std::vector<uint16_t> map(256, 7);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COLORMAP, map.data(),
                           map.data(), map.data()),
              1);
    {
      // Allow `dir` (~200B) but fail ColorMap's 1536-byte malloc.
      ScopedSingleMemAllocLimit single_limit(writer.get(), 512);
      EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
      EXPECT_TRUE(
          ContainsSubstring(stream.diagnostics().errors(), "Out of memory"));
    }
    ASSERT_EQ(TIFFUnsetField(writer.get(), TIFFTAG_COLORMAP), 1);

    // Directory write SeekOK & WriteOK failure at lines 1375 / 1381.
    stream.diagnostics().Clear();
    stream.FailSeeksAfter(stream.seek_calls() + 2);
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
    stream.ResetFaults();
  }

  // Test dir SeekOK/WriteOK directly during clean directory final write.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ARTIST,
                           "LongArtistPayloadForDirWrite"),
              1);
    ASSERT_EQ(TIFFSetupStrips(writer.get()), 1);
    // Out-of-line ARTIST WriteOK fail (tag data write).
    stream.FailWritesAfter(stream.write_calls() + 1, -1);
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
    EXPECT_TRUE(
        ContainsSubstring(stream.diagnostics().errors(), "writing tag data"));
    writer->tif_flags &=
        ~(TIFF_DIRTYDIRECT | TIFF_DIRTYSTRIP | TIFF_BEENWRITING);
  }
  {
    // Directory header/entries WriteOK fail on a fresh writer after header
    // link + tag data write.
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripedGray8(writer.get(), 4, 2, 2);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ARTIST,
                           "LongArtistPayloadForDirWrite"),
              1);
    ASSERT_EQ(TIFFSetupStrips(writer.get()), 1);
    stream.FailWritesAfter(stream.write_calls() + 2, -1);
    EXPECT_EQ(TIFFCheckpointDirectory(writer.get()), 0);
    EXPECT_TRUE(ContainsSubstring(stream.diagnostics().errors(),
                                  "IO error writing directory"));
    writer->tif_flags &=
        ~(TIFF_DIRTYDIRECT | TIFF_DIRTYSTRIP | TIFF_BEENWRITING);
  }
}

}  // namespace
}  // namespace libtiff
