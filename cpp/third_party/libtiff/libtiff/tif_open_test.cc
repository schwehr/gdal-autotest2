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

// Unit tests for libtiff tif_open.c (TIFFOpenOptions, _TIFFgetMode,
// _TIFF*Ext allocators, TIFFClientOpen/Ext, mode flags, header validation,
// and TIFF handle property getters/setters).

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
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

struct GlobalDiagnosticCapture {
  static void Clear() {
    errors().clear();
    warnings().clear();
  }

  static void OnError(const char* module, const char* fmt, va_list ap) {
    errors().push_back({
        .module = module != nullptr ? module : "",
        .message = Format(fmt, ap),
    });
  }

  static void OnWarning(const char* module, const char* fmt, va_list ap) {
    warnings().push_back({
        .module = module != nullptr ? module : "",
        .message = Format(fmt, ap),
    });
  }

  struct Entry {
    std::string module;
    std::string message;
  };

  static std::vector<Entry>& errors() {
    static auto* v = new std::vector<Entry>();
    return *v;
  }

  static std::vector<Entry>& warnings() {
    static auto* v = new std::vector<Entry>();
    return *v;
  }

 private:
  static std::string Format(const char* fmt, va_list ap) {
    if (fmt == nullptr) return {};
    va_list copy;
    va_copy(copy, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (n < 0) return {};
    std::string s(static_cast<size_t>(n) + 1, '\0');
    std::vsnprintf(s.data(), s.size(), fmt, ap);
    s.resize(static_cast<size_t>(n));
    return s;
  }
};

// RAII owner for blocks allocated via _TIFFmallocExt / _TIFFcallocExt /
// _TIFFreallocExt so they are always freed with _TIFFfreeExt before TIFFClose.
class ScopedTiffExtAlloc {
 public:
  ScopedTiffExtAlloc(TIFF* tif, void* ptr) noexcept : tif_(tif), ptr_(ptr) {}
  ~ScopedTiffExtAlloc() { reset(); }

  ScopedTiffExtAlloc(const ScopedTiffExtAlloc&) = delete;
  ScopedTiffExtAlloc& operator=(const ScopedTiffExtAlloc&) = delete;

  void* get() const { return ptr_; }

  void reset(void* next = nullptr) noexcept {
    if (ptr_ != nullptr) {
      _TIFFfreeExt(tif_, ptr_);
    }
    ptr_ = next;
  }

  void* release() noexcept {
    void* p = ptr_;
    ptr_ = nullptr;
    return p;
  }

 private:
  TIFF* tif_;
  void* ptr_;
};

std::vector<uint8_t> BuildMinimalStripTiff(bool big_endian, bool big_tiff,
                                           uint32_t width = 4,
                                           uint32_t height = 2) {
  RawTiffBuilder builder(big_endian, big_tiff);
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height, 0x5a);
  const uint64_t strip_off = builder.AppendBlob(pixels);

  std::vector<RawIfdEntry> entries;
  entries.push_back(builder.MakeLong(TIFFTAG_IMAGEWIDTH, width));
  entries.push_back(builder.MakeLong(TIFFTAG_IMAGELENGTH, height));
  entries.push_back(builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
  entries.push_back(builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
  entries.push_back(
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
  if (big_tiff) {
    entries.push_back(builder.MakeLong8(TIFFTAG_STRIPOFFSETS, strip_off));
  } else {
    entries.push_back(builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                                       static_cast<uint32_t>(strip_off)));
  }
  entries.push_back(builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1));
  entries.push_back(builder.MakeLong(TIFFTAG_ROWSPERSTRIP, height));
  if (big_tiff) {
    entries.push_back(
        builder.MakeLong8(TIFFTAG_STRIPBYTECOUNTS, pixels.size()));
  } else {
    entries.push_back(builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                                       static_cast<uint32_t>(pixels.size())));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

std::vector<uint8_t> BuildMinimalTiledTiff(bool big_endian, bool big_tiff) {
  RawTiffBuilder builder(big_endian, big_tiff);
  std::vector<uint8_t> tile(16 * 16, 0x33);
  const uint64_t tile_off = builder.AppendBlob(tile);

  std::vector<RawIfdEntry> entries;
  entries.push_back(builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16));
  entries.push_back(builder.MakeLong(TIFFTAG_IMAGELENGTH, 16));
  entries.push_back(builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
  entries.push_back(builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
  entries.push_back(
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
  entries.push_back(builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1));
  entries.push_back(builder.MakeLong(TIFFTAG_TILEWIDTH, 16));
  entries.push_back(builder.MakeLong(TIFFTAG_TILELENGTH, 16));
  if (big_tiff) {
    entries.push_back(builder.MakeLong8(TIFFTAG_TILEOFFSETS, tile_off));
    entries.push_back(builder.MakeLong8(TIFFTAG_TILEBYTECOUNTS, tile.size()));
  } else {
    entries.push_back(
        builder.MakeLong(TIFFTAG_TILEOFFSETS, static_cast<uint32_t>(tile_off)));
    entries.push_back(builder.MakeLong(TIFFTAG_TILEBYTECOUNTS,
                                       static_cast<uint32_t>(tile.size())));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

void WriteSimpleStripDirectory(TIFF* tif, uint32_t width, uint32_t height,
                               uint8_t fill_value) {
  ASSERT_NE(tif, nullptr);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, height), 1);

  std::vector<uint8_t> strip(static_cast<size_t>(width) * height, fill_value);
  ASSERT_EQ(TIFFWriteEncodedStrip(tif, 0, strip.data(),
                                  static_cast<tmsize_t>(strip.size())),
            static_cast<tmsize_t>(strip.size()));
}

TEST(TifOpenTest, DummyMapAndUnmapProcedures) {
  uint8_t dummy_byte = 0;
  void* base = &dummy_byte;
  toff_t size = 99;
  EXPECT_EQ(_tiffDummyMapProc(nullptr, &base, &size), 0);
  EXPECT_EQ(base, &dummy_byte);
  EXPECT_EQ(size, 99);

  // _tiffDummyUnmapProc is a safe no-op.
  _tiffDummyUnmapProc(nullptr, nullptr, 0);
  _tiffDummyUnmapProc(nullptr, base, size);
}

TEST(TifOpenTest, GetModeValidAndInvalid) {
  struct ModeCase {
    const char* mode;
    int expected;
  };
  constexpr std::array<ModeCase, 9> kValidCases = {{
      {.mode = "r", .expected = O_RDONLY},
      {.mode = "rb", .expected = O_RDONLY},
      {.mode = "r8mC", .expected = O_RDONLY},
      {.mode = "r+", .expected = O_RDWR},
      {.mode = "r+l", .expected = O_RDWR},
      {.mode = "w", .expected = O_RDWR | O_CREAT | O_TRUNC},
      {.mode = "w8b", .expected = O_RDWR | O_CREAT | O_TRUNC},
      {.mode = "a", .expected = O_RDWR | O_CREAT},
      {.mode = "a8", .expected = O_RDWR | O_CREAT},
  }};

  DiagnosticRecorder diag;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  diag.Install(opts.get());

  for (const auto& tc : kValidCases) {
    SCOPED_TRACE(tc.mode);
    EXPECT_EQ(_TIFFgetMode(opts.get(), nullptr, tc.mode, "GetModeTest"),
              tc.expected);
  }
  EXPECT_EQ(diag.error_count(), 0U);

  constexpr std::array<const char*, 6> kInvalidModes = {
      "", "x", "z", "+", "R", "W",
  };
  for (const char* bad : kInvalidModes) {
    SCOPED_TRACE(bad);
    diag.Clear();
    EXPECT_EQ(_TIFFgetMode(opts.get(), nullptr, bad, "GetModeTest"), -1);
    const auto errors = diag.errors();
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].tif, nullptr);
    EXPECT_EQ(errors[0].module, "GetModeTest");
    EXPECT_THAT(errors[0].message, HasSubstr("Bad mode"));
  }

  // Also test _TIFFgetMode with opts == nullptr using scoped global handler.
  GlobalDiagnosticCapture::Clear();
  ScopedTiffErrorHandlers global_guard(&GlobalDiagnosticCapture::OnError,
                                       &GlobalDiagnosticCapture::OnWarning);
  EXPECT_EQ(_TIFFgetMode(nullptr, nullptr, "invalid", "GlobalModeTest"), -1);
  ASSERT_EQ(GlobalDiagnosticCapture::errors().size(), 1U);
  EXPECT_EQ(GlobalDiagnosticCapture::errors()[0].module, "GlobalModeTest");
  EXPECT_THAT(GlobalDiagnosticCapture::errors()[0].message,
              HasSubstr("Bad mode"));
}

TEST(TifOpenTest, OpenOptionsLifecycleAndUnknownTagWarning) {
  // TIFFOpenOptionsFree(nullptr) must be a safe no-op.
  TIFFOpenOptionsFree(nullptr);

  // Build an image containing an unregistered custom tag 65000.
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  const std::array<uint8_t, 4> pixels = {1, 2, 3, 4};
  const uint64_t strip_off = builder.AppendBlob(pixels);
  builder.AddIfd({
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
      builder.MakeLong(65000, 12345),
  });

  // Default: WarnAboutUnknownTags is disabled (0).
  {
    MemoryTiffStream stream(builder.bytes());
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    diag.Install(opts.get());
    TIFFOpenOptionsSetWarnAboutUnknownTags(opts.get(), 0);

    UniqueTiff tif = stream.Open("r", opts.get(), "no_warn.tiff");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(diag.warning_count(), 0U);
  }

  // Enabled: WarnAboutUnknownTags == 1 triggers unknown tag warning.
  {
    MemoryTiffStream stream(builder.bytes());
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    diag.Install(opts.get());
    TIFFOpenOptionsSetWarnAboutUnknownTags(opts.get(), 1);

    UniqueTiff tif = stream.Open("r", opts.get(), "warn_unknown.tiff");
    ASSERT_NE(tif, nullptr);
    const auto warnings = diag.warnings();
    ASSERT_FALSE(warnings.empty());
    EXPECT_EQ(warnings[0].tif, tif.get());
    EXPECT_THAT(warnings[0].message, HasSubstr("Unknown field"));
  }
}

TEST(TifOpenTest, EarlyAllocationLimitsAndBadModeRejectBeforeOpeningHandle) {
  MemoryTiffStream stream;
  DiagnosticRecorder diag;

  // Invalid mode rejected by TIFFClientOpenExt before allocating TIFF struct.
  {
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    diag.Install(opts.get());

    UniqueTiff tif = stream.Open("x", opts.get(), "bad_mode.tiff");
    EXPECT_EQ(tif, nullptr);
    const auto errors = diag.errors();
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].tif, nullptr);
    EXPECT_EQ(errors[0].module, "TIFFClientOpenExt");
    EXPECT_THAT(errors[0].message, HasSubstr("Bad mode"));
  }

  // Invalid mode via TIFFClientOpen (opts == nullptr) chains to global handler.
  {
    GlobalDiagnosticCapture::Clear();
    ScopedTiffErrorHandlers global_guard(&GlobalDiagnosticCapture::OnError,
                                         &GlobalDiagnosticCapture::OnWarning);
    UniqueTiff tif(TIFFClientOpen(
        "bad_mode_no_opts.tiff", "", static_cast<thandle_t>(&stream),
        &MemoryTiffStream::ReadProc, &MemoryTiffStream::WriteProc,
        &MemoryTiffStream::SeekProc, &MemoryTiffStream::CloseProc,
        &MemoryTiffStream::SizeProc, nullptr, nullptr));
    EXPECT_EQ(tif, nullptr);
    ASSERT_EQ(GlobalDiagnosticCapture::errors().size(), 1U);
    EXPECT_EQ(GlobalDiagnosticCapture::errors()[0].module, "TIFFClientOpenExt");
    EXPECT_THAT(GlobalDiagnosticCapture::errors()[0].message,
                HasSubstr("Bad mode"));
  }

  // Exceeding MaxSingleMemAlloc for the initial TIFF structure itself.
  {
    diag.Clear();
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    diag.Install(opts.get());
    TIFFOpenOptionsSetMaxSingleMemAlloc(opts.get(), 16);

    UniqueTiff tif = stream.Open("w", opts.get(), "single_limit.tiff");
    EXPECT_EQ(tif, nullptr);
    const auto errors = diag.errors();
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].tif, nullptr);
    EXPECT_EQ(errors[0].module, "TIFFClientOpenExt");
    EXPECT_THAT(errors[0].message, HasSubstr("byte limit"));
  }

  // Exceeding MaxCumulatedMemAlloc for the initial TIFF structure itself.
  {
    diag.Clear();
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    diag.Install(opts.get());
    TIFFOpenOptionsSetMaxSingleMemAlloc(opts.get(), 0);
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 16);

    UniqueTiff tif = stream.Open("w", opts.get(), "cumul_limit.tiff");
    EXPECT_EQ(tif, nullptr);
    const auto errors = diag.errors();
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].tif, nullptr);
    EXPECT_EQ(errors[0].module, "TIFFClientOpenExt");
    EXPECT_THAT(errors[0].message, HasSubstr("cumulated byte limit"));
  }
}

TEST(TifOpenTest, ExtAllocatorsWithNullAndUnlimitedHandle) {
  // tif == nullptr routes directly to
  // _TIFFmalloc/_TIFFcalloc/_TIFFrealloc/_TIFFfree.
  {
    ScopedTiffExtAlloc p(nullptr, _TIFFmallocExt(nullptr, 32));
    ASSERT_NE(p.get(), nullptr);
    std::memset(p.get(), 0xab, 32);

    void* grown = _TIFFreallocExt(nullptr, p.get(), 64);
    ASSERT_NE(grown, nullptr);
    p.release();
    p.reset(grown);
    EXPECT_EQ(static_cast<uint8_t*>(p.get())[0], 0xab);

    ScopedTiffExtAlloc c(nullptr, _TIFFcallocExt(nullptr, 4, 8));
    ASSERT_NE(c.get(), nullptr);
    for (int i = 0; i < 32; ++i) {
      EXPECT_EQ(static_cast<uint8_t*>(c.get())[i], 0);
    }

    _TIFFfreeExt(nullptr, nullptr);
  }

  // Invalid _TIFFcallocExt arguments return nullptr even with tif == nullptr.
  EXPECT_EQ(_TIFFcallocExt(nullptr, 0, 8), nullptr);
  EXPECT_EQ(_TIFFcallocExt(nullptr, -1, 8), nullptr);
  EXPECT_EQ(_TIFFcallocExt(nullptr, 8, 0), nullptr);
  EXPECT_EQ(_TIFFcallocExt(nullptr, 8, -4), nullptr);
  EXPECT_EQ(_TIFFcallocExt(nullptr, TIFF_TMSIZE_T_MAX, 2), nullptr);

  // Non-null handle with unlimited options (max_single == 0, max_cumulated ==
  // 0).
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  ScopedTiffExtAlloc m_alloc(tif.get(), _TIFFmallocExt(tif.get(), 48));
  ASSERT_NE(m_alloc.get(), nullptr);
  void* realloced = _TIFFreallocExt(tif.get(), m_alloc.get(), 96);
  ASSERT_NE(realloced, nullptr);
  m_alloc.release();
  m_alloc.reset(realloced);

  ScopedTiffExtAlloc c_alloc(tif.get(), _TIFFcallocExt(tif.get(), 3, 16));
  ASSERT_NE(c_alloc.get(), nullptr);
  _TIFFfreeExt(tif.get(), nullptr);
}

TEST(TifOpenTest, ExtAllocatorsSingleMemAllocLimit) {
  MemoryTiffStream stream;
  DiagnosticRecorder diag;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  diag.Install(opts.get());
  TIFFOpenOptionsSetMaxSingleMemAlloc(opts.get(), 1 << 20);

  UniqueTiff tif = stream.Open("w", opts.get());
  ASSERT_NE(tif, nullptr);
  EXPECT_EQ(tif->tif_max_single_mem_alloc, 1 << 20);

  {
    ScopedSingleMemAllocLimit limit(tif.get(), 64);

    // _TIFFmallocExt within and beyond limit.
    ScopedTiffExtAlloc ok_m(tif.get(), _TIFFmallocExt(tif.get(), 64));
    EXPECT_NE(ok_m.get(), nullptr);
    EXPECT_EQ(_TIFFmallocExt(tif.get(), 65), nullptr);

    // _TIFFcallocExt within and beyond limit.
    ScopedTiffExtAlloc ok_c(tif.get(), _TIFFcallocExt(tif.get(), 8, 8));
    EXPECT_NE(ok_c.get(), nullptr);
    EXPECT_EQ(_TIFFcallocExt(tif.get(), 9, 8), nullptr);

    // _TIFFreallocExt within and beyond limit (must preserve original pointer
    // on fail).
    void* grown = _TIFFreallocExt(tif.get(), ok_m.get(), 64);
    ASSERT_NE(grown, nullptr);
    ok_m.release();
    ok_m.reset(grown);

    EXPECT_EQ(_TIFFreallocExt(tif.get(), ok_m.get(), 65), nullptr);
  }

  EXPECT_GE(diag.error_count(), 3U);
}

TEST(TifOpenTest, ExtAllocatorsCumulatedMemAllocLimit) {
  MemoryTiffStream stream;
  DiagnosticRecorder diag;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  diag.Install(opts.get());
  // Must open with > 0 cumulated limit before tightening with
  // ScopedCumulatedMemAllocRemaining.
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

  UniqueTiff tif = stream.Open("w", opts.get());
  ASSERT_NE(tif, nullptr);
  EXPECT_EQ(tif->tif_max_cumulated_mem_alloc, 1 << 24);

  {
    ScopedCumulatedMemAllocRemaining budget(tif.get(), 128);
    const tmsize_t base_cur = tif->tif_cur_cumulated_mem_alloc;

    ScopedTiffExtAlloc a1(tif.get(), _TIFFmallocExt(tif.get(), 48));
    ASSERT_NE(a1.get(), nullptr);
    EXPECT_EQ(tif->tif_cur_cumulated_mem_alloc, base_cur + 48);

    ScopedTiffExtAlloc a2(tif.get(), _TIFFcallocExt(tif.get(), 4, 16));
    ASSERT_NE(a2.get(), nullptr);
    EXPECT_EQ(tif->tif_cur_cumulated_mem_alloc, base_cur + 112);

    // Exceeding remaining budget (16 bytes left) fails for malloc and calloc.
    EXPECT_EQ(_TIFFmallocExt(tif.get(), 17), nullptr);
    EXPECT_EQ(_TIFFcallocExt(tif.get(), 3, 6), nullptr);

    // Realloc with nullptr works within remaining budget.
    ScopedTiffExtAlloc a3(tif.get(), _TIFFreallocExt(tif.get(), nullptr, 16));
    ASSERT_NE(a3.get(), nullptr);
    EXPECT_EQ(tif->tif_cur_cumulated_mem_alloc, base_cur + 128);

    // Growing a3 with 0 remaining fails and keeps old allocation intact.
    EXPECT_EQ(_TIFFreallocExt(tif.get(), a3.get(), 24), nullptr);
    EXPECT_EQ(tif->tif_cur_cumulated_mem_alloc, base_cur + 128);

    // Shrinking a1 succeeds even with 0 remaining budget, freeing 16 bytes.
    void* shrunk = _TIFFreallocExt(tif.get(), a1.get(), 32);
    ASSERT_NE(shrunk, nullptr);
    a1.release();
    a1.reset(shrunk);
    EXPECT_EQ(tif->tif_cur_cumulated_mem_alloc, base_cur + 112);

    // Now growing a3 from 16 to 32 (+16 bytes) fits exactly.
    void* grown = _TIFFreallocExt(tif.get(), a3.get(), 32);
    ASSERT_NE(grown, nullptr);
    a3.release();
    a3.reset(grown);
    EXPECT_EQ(tif->tif_cur_cumulated_mem_alloc, base_cur + 128);

    // Freeing restores tracked total.
    a3.reset();
    a2.reset();
    a1.reset();
    EXPECT_EQ(tif->tif_cur_cumulated_mem_alloc, base_cur);
  }

  // Test the TIFF_TMSIZE_T_MAX - LEADING_AREA_TO_STORE_ALLOC_SIZE overflow
  // guard when max_cumulated_mem_alloc == TIFF_TMSIZE_T_MAX and max_single ==
  // 0.
  {
    const tmsize_t prev_cumul = tif->tif_max_cumulated_mem_alloc;
    tif->tif_max_cumulated_mem_alloc = TIFF_TMSIZE_T_MAX;
    const auto restore_cumul = absl::MakeCleanup(
        [&] { tif->tif_max_cumulated_mem_alloc = prev_cumul; });

    EXPECT_EQ(_TIFFmallocExt(tif.get(), TIFF_TMSIZE_T_MAX), nullptr);
    // 2 * (TIFF_TMSIZE_T_MAX / 2) == TIFF_TMSIZE_T_MAX - 1 (even), which passes
    // nmemb <= TIFF_TMSIZE_T_MAX / siz but exceeds TIFF_TMSIZE_T_MAX - 16.
    EXPECT_EQ(_TIFFcallocExt(tif.get(), 2, TIFF_TMSIZE_T_MAX / 2), nullptr);
    EXPECT_EQ(_TIFFreallocExt(tif.get(), nullptr, TIFF_TMSIZE_T_MAX), nullptr);

    // Passing s == -2 * sizeof(size_t) makes LEADING_AREA + s == 0, which makes
    // _TIFFmalloc(0) return nullptr cleanly without triggering OOM abort.
    const tmsize_t neg_leading = -static_cast<tmsize_t>(2 * sizeof(size_t));
    EXPECT_EQ(_TIFFmallocExt(tif.get(), neg_leading), nullptr);
  }

  EXPECT_GE(diag.error_count(), 6U);
}

TEST(TifOpenTest, NullRequiredClientProcsFailCleanly) {
  enum class MissingProc { kRead, kWrite, kSeek, kClose, kSize };
  constexpr std::array<MissingProc, 5> kCases = {
      MissingProc::kRead,  MissingProc::kWrite, MissingProc::kSeek,
      MissingProc::kClose, MissingProc::kSize,
  };

  for (MissingProc missing : kCases) {
    MemoryTiffStream stream;
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    diag.Install(opts.get());

    TIFF* raw = TIFFClientOpenExt(
        "null_proc.tiff", "w", static_cast<thandle_t>(&stream),
        missing == MissingProc::kRead ? nullptr : &MemoryTiffStream::ReadProc,
        missing == MissingProc::kWrite ? nullptr : &MemoryTiffStream::WriteProc,
        missing == MissingProc::kSeek ? nullptr : &MemoryTiffStream::SeekProc,
        missing == MissingProc::kClose ? nullptr : &MemoryTiffStream::CloseProc,
        missing == MissingProc::kSize ? nullptr : &MemoryTiffStream::SizeProc,
        nullptr, nullptr, opts.get());
    UniqueTiff tif(raw);
    EXPECT_EQ(tif, nullptr);
    EXPECT_EQ(stream.close_calls(), 0U);
    const auto errors = diag.errors();
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_NE(errors[0].tif, nullptr);
    EXPECT_THAT(errors[0].message, HasSubstr("NULL pointer"));
  }
}

TEST(TifOpenTest, ClientOpenWithoutOptionsClassicAndBigTiff) {
  GlobalDiagnosticCapture::Clear();
  ScopedTiffErrorHandlers global_guard(&GlobalDiagnosticCapture::OnError,
                                       &GlobalDiagnosticCapture::OnWarning);

  for (bool big_tiff : {false, true}) {
    SCOPED_TRACE(big_tiff);
    MemoryTiffStream stream;
    {
      UniqueTiff writer(TIFFClientOpen(
          "legacy.tiff", big_tiff ? "w8" : "w", static_cast<thandle_t>(&stream),
          &MemoryTiffStream::ReadProc, &MemoryTiffStream::WriteProc,
          &MemoryTiffStream::SeekProc, &MemoryTiffStream::CloseProc,
          &MemoryTiffStream::SizeProc, nullptr, nullptr));
      ASSERT_NE(writer, nullptr);
      EXPECT_EQ(TIFFIsBigTIFF(writer.get()), big_tiff ? 1 : 0);
      EXPECT_EQ(TIFFGetMapFileProc(writer.get()), &_tiffDummyMapProc);
      EXPECT_EQ(TIFFGetUnmapFileProc(writer.get()), &_tiffDummyUnmapProc);
      WriteSimpleStripDirectory(writer.get(), 4, 2, 0x77);
    }
    EXPECT_TRUE(stream.is_closed());
    EXPECT_TRUE(GlobalDiagnosticCapture::errors().empty());
    EXPECT_TRUE(GlobalDiagnosticCapture::warnings().empty());

    stream.ResetPosition();
    UniqueTiff reader(TIFFClientOpen(
        "legacy.tiff", "r", static_cast<thandle_t>(&stream),
        &MemoryTiffStream::ReadProc, &MemoryTiffStream::WriteProc,
        &MemoryTiffStream::SeekProc, &MemoryTiffStream::CloseProc,
        &MemoryTiffStream::SizeProc, nullptr, nullptr));
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFIsBigTIFF(reader.get()), big_tiff ? 1 : 0);
    uint32_t w = 0;
    uint32_t h = 0;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGEWIDTH, &w), 1);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGELENGTH, &h), 1);
    EXPECT_EQ(w, 4U);
    EXPECT_EQ(h, 2U);
    std::vector<uint8_t> strip(8);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip.data(),
                                   static_cast<tmsize_t>(strip.size())),
              8);
    EXPECT_EQ(strip[0], 0x77);
    EXPECT_EQ(strip[7], 0x77);
  }
}

TEST(TifOpenTest, ModeFlagsEndiannessBitnessBitOrderMmapStripchopDefer) {
  // Classic little-endian ("wl" and "w4").
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w4l");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFIsBigEndian(tif.get()), 0);
    EXPECT_EQ(TIFFIsByteSwapped(tif.get()), 0);
    EXPECT_EQ(TIFFIsBigTIFF(tif.get()), 0);
    EXPECT_EQ(TIFFIsMSB2LSB(tif.get()), 1);
    EXPECT_EQ(tif->tif_header.common.tiff_magic, TIFF_LITTLEENDIAN);
    EXPECT_EQ(tif->tif_header.common.tiff_version, TIFF_VERSION_CLASSIC);
    WriteSimpleStripDirectory(tif.get(), 2, 2, 0x11);
    tif.reset();
    ASSERT_GE(stream.data().size(), 8U);
    EXPECT_EQ(stream.data()[0], 'I');
    EXPECT_EQ(stream.data()[1], 'I');
    EXPECT_EQ(stream.data()[2], 42);
    EXPECT_EQ(stream.data()[3], 0);
  }

  // Classic big-endian ("wb").
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wb");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFIsBigEndian(tif.get()), 1);
    EXPECT_EQ(TIFFIsByteSwapped(tif.get()), 1);
    EXPECT_EQ(TIFFIsBigTIFF(tif.get()), 0);
    EXPECT_EQ(tif->tif_header.common.tiff_magic, TIFF_BIGENDIAN);
    EXPECT_EQ(tif->tif_header.common.tiff_version, TIFF_VERSION_CLASSIC);
    WriteSimpleStripDirectory(tif.get(), 2, 2, 0x22);
    tif.reset();
    ASSERT_GE(stream.data().size(), 8U);
    EXPECT_EQ(stream.data()[0], 'M');
    EXPECT_EQ(stream.data()[1], 'M');
    EXPECT_EQ(stream.data()[2], 0);
    EXPECT_EQ(stream.data()[3], 42);
  }

  // BigTIFF little-endian ("w8l") and big-endian ("w8b").
  for (bool be : {false, true}) {
    SCOPED_TRACE(be);
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open(be ? "w8b" : "w8l");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFIsBigEndian(tif.get()), be ? 1 : 0);
    EXPECT_EQ(TIFFIsByteSwapped(tif.get()), be ? 1 : 0);
    EXPECT_EQ(TIFFIsBigTIFF(tif.get()), 1);
    EXPECT_EQ(tif->tif_header_size, sizeof(TIFFHeaderBig));
    EXPECT_EQ(tif->tif_header.big.tiff_offsetsize, 8);
    EXPECT_EQ(tif->tif_header.big.tiff_unused, 0);
    WriteSimpleStripDirectory(tif.get(), 2, 2, 0x44);
    tif.reset();

    ASSERT_GE(stream.data().size(), sizeof(TIFFHeaderBig));
    EXPECT_EQ(stream.data()[0], be ? 'M' : 'I');
    EXPECT_EQ(stream.data()[1], be ? 'M' : 'I');
    if (be) {
      EXPECT_EQ(stream.data()[2], 0);
      EXPECT_EQ(stream.data()[3], 43);
      EXPECT_EQ(stream.data()[4], 0);
      EXPECT_EQ(stream.data()[5], 8);
    } else {
      EXPECT_EQ(stream.data()[2], 43);
      EXPECT_EQ(stream.data()[3], 0);
      EXPECT_EQ(stream.data()[4], 8);
      EXPECT_EQ(stream.data()[5], 0);
    }
  }

  // Bit order flags 'L', 'B', 'H' (with deprecation warning on 'H').
  {
    MemoryTiffStream stream(BuildMinimalStripTiff(false, false));
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    diag.Install(opts.get());

    UniqueTiff tif_lsb = stream.Open("rL", opts.get());
    ASSERT_NE(tif_lsb, nullptr);
    EXPECT_EQ(TIFFIsMSB2LSB(tif_lsb.get()), 0);
    EXPECT_EQ(diag.warning_count(), 0U);

    UniqueTiff tif_msb = stream.Open("rLB", opts.get());
    ASSERT_NE(tif_msb, nullptr);
    EXPECT_EQ(TIFFIsMSB2LSB(tif_msb.get()), 1);
    EXPECT_EQ(diag.warning_count(), 0U);

    UniqueTiff tif_host = stream.Open("rH", opts.get());
    ASSERT_NE(tif_host, nullptr);
    EXPECT_EQ(TIFFIsMSB2LSB(tif_host.get()), 1);
    ASSERT_EQ(diag.warning_count(), 1U);
    EXPECT_THAT(diag.warnings()[0].message, HasSubstr("deprecated"));

    // Returning 0 from WarningHandlerExtR chains to global warning handler.
    GlobalDiagnosticCapture::Clear();
    ScopedTiffErrorHandlers global_guard(&GlobalDiagnosticCapture::OnError,
                                         &GlobalDiagnosticCapture::OnWarning);
    diag.Clear();
    diag.set_chain_to_global(true);
    UniqueTiff tif_chained = stream.Open("rH", opts.get());
    ASSERT_NE(tif_chained, nullptr);
    EXPECT_EQ(diag.warning_count(), 1U);
    ASSERT_EQ(GlobalDiagnosticCapture::warnings().size(), 1U);
    EXPECT_THAT(GlobalDiagnosticCapture::warnings()[0].message,
                HasSubstr("deprecated"));
  }

  // 'M'/'m' mmap, 'C'/'c' stripchop, 'D'/'O' defer/lazy strile load, 'h'
  // header-only.
  {
    const std::vector<uint8_t> raw = BuildMinimalStripTiff(false, false);

    // 'M' enabled with MapProc present.
    {
      MemoryTiffStream stream(raw);
      stream.set_enable_mmap(true);
      UniqueTiff tif = stream.Open("rmM");
      ASSERT_NE(tif, nullptr);
      EXPECT_NE(tif->tif_flags & TIFF_MAPPED, 0U);
      EXPECT_EQ(stream.map_calls(), 1U);
      EXPECT_NE(tif->tif_flags & TIFF_STRIPCHOP, 0U);
      tif.reset();
      EXPECT_EQ(stream.unmap_calls(), 1U);
    }

    // 'm' disables mmap, 'c' disables stripchop, 'O' sets lazy+defer.
    {
      MemoryTiffStream stream(raw);
      stream.set_enable_mmap(true);
      UniqueTiff tif = stream.Open("rmcO");
      ASSERT_NE(tif, nullptr);
      EXPECT_EQ(tif->tif_flags & TIFF_MAPPED, 0U);
      EXPECT_EQ(stream.map_calls(), 0U);
      EXPECT_EQ(tif->tif_flags & TIFF_STRIPCHOP, 0U);
      EXPECT_NE(tif->tif_flags & TIFF_DEFERSTRILELOAD, 0U);
      EXPECT_NE(tif->tif_flags & TIFF_LAZYSTRILELOAD_ASKED, 0U);
    }

    // 'C' re-enables stripchop after 'c', 'D' enables defer load, unknown flag
    // 'X' ignored.
    {
      MemoryTiffStream stream(raw);
      UniqueTiff tif = stream.Open("rcCDX");
      ASSERT_NE(tif, nullptr);
      EXPECT_NE(tif->tif_flags & TIFF_STRIPCHOP, 0U);
      EXPECT_NE(tif->tif_flags & TIFF_DEFERSTRILELOAD, 0U);
      EXPECT_EQ(tif->tif_flags & TIFF_LAZYSTRILELOAD_ASKED, 0U);
    }

    // Non-O_RDONLY modes ignore 'M', 'm', 'C', 'c', 'O', 'b'/'8' on 'r+'.
    {
      MemoryTiffStream stream(raw);
      stream.set_enable_mmap(true);
      UniqueTiff tif = stream.Open("r+MCcOb8");
      ASSERT_NE(tif, nullptr);
      EXPECT_EQ(tif->tif_flags & TIFF_MAPPED, 0U);
      EXPECT_EQ(stream.map_calls(), 0U);
      EXPECT_EQ(tif->tif_flags & TIFF_LAZYSTRILELOAD_ASKED, 0U);
      EXPECT_EQ(TIFFIsBigTIFF(tif.get()), 0);
      EXPECT_EQ(TIFFIsByteSwapped(tif.get()), 0);
    }

    // 'h' (TIFF_HEADERONLY): opens header even when first IFD offset is 0 /
    // empty.
    {
      RawTiffBuilder bad_dir_builder(/*big_endian=*/true, /*big_tiff=*/false);
      // Header only, first IFD offset = 0 (would fail normal
      // TIFFReadDirectory).
      MemoryTiffStream stream(bad_dir_builder.bytes());
      UniqueTiff tif = stream.Open("rh");
      ASSERT_NE(tif, nullptr);
      EXPECT_EQ(TIFFIsBigEndian(tif.get()), 1);
      EXPECT_EQ(TIFFIsByteSwapped(tif.get()), 1);
      EXPECT_EQ(TIFFCurrentDirectory(tif.get()), TIFF_NON_EXISTENT_DIR_NUMBER);
    }
  }
}

TEST(TifOpenTest, AppendAndReadWriteCreateOrPreserveExistingFile) {
  // Append mode on an empty stream initializes a new TIFF header.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("a");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(tif->tif_curdircount, 0U);
    WriteSimpleStripDirectory(tif.get(), 2, 2, 0xaa);
    EXPECT_EQ(TIFFWriteDirectory(tif.get()), 1);
    tif.reset();

    // Open again with "a" on existing non-empty file: hits `case 'a':`!
    stream.ResetPosition();
    UniqueTiff appender = stream.Open("a");
    ASSERT_NE(appender, nullptr);
    EXPECT_EQ(TIFFCurrentDirectory(appender.get()),
              TIFF_NON_EXISTENT_DIR_NUMBER);
    WriteSimpleStripDirectory(appender.get(), 3, 2, 0xbb);
    EXPECT_EQ(TIFFWriteDirectory(appender.get()), 1);
    appender.reset();

    // Verify 2 directories exist.
    stream.ResetPosition();
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFNumberOfDirectories(reader.get()), 2U);
    EXPECT_EQ(TIFFCurrentDirectory(reader.get()), 0U);
    EXPECT_EQ(TIFFReadDirectory(reader.get()), 1);
    EXPECT_EQ(TIFFCurrentDirectory(reader.get()), 1U);
  }

  // Read-write "r+" on an empty stream creates a new header because ReadOK
  // fails.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("r+");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(tif->tif_mode, O_RDWR);
    WriteSimpleStripDirectory(tif.get(), 2, 2, 0xcc);
    EXPECT_EQ(TIFFWriteDirectory(tif.get()), 1);
    tif.reset();

    stream.ResetPosition();
    UniqueTiff reader = stream.Open("r+");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFCurrentDirectory(reader.get()), 0U);
    EXPECT_EQ(TIFFGetMode(reader.get()), O_RDWR);
  }
}

TEST(TifOpenTest, MdiLittleEndianMagicAccepted) {
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  const std::array<uint8_t, 4> pixels = {1, 2, 3, 4};
  const uint64_t strip_off = builder.AppendBlob(pixels);
  builder.AddIfd({
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
  });
  std::vector<uint8_t> bytes = builder.Take();
  // Replace 'I','I' with MDI_LITTLEENDIAN (0x5045 -> 'E','P').
  bytes[0] = static_cast<uint8_t>(MDI_LITTLEENDIAN & 0xffU);
  bytes[1] = static_cast<uint8_t>((MDI_LITTLEENDIAN >> 8) & 0xffU);

  MemoryTiffStream stream(std::move(bytes));
  UniqueTiff tif = stream.Open("r");
  ASSERT_NE(tif, nullptr);
  EXPECT_EQ(TIFFIsBigEndian(tif.get()), 0);
  EXPECT_EQ(TIFFIsByteSwapped(tif.get()), 0);
  uint32_t width = 0;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &width), 1);
  EXPECT_EQ(width, 2U);
}

TEST(TifOpenTest, HeaderAndIoFailurePaths) {
  // Empty and short headers in read-only mode.
  for (size_t len : {size_t{0}, size_t{4}, size_t{7}}) {
    SCOPED_TRACE(len);
    MemoryTiffStream stream(std::vector<uint8_t>(len, 'I'));
    UniqueTiff tif = stream.Open("r");
    EXPECT_EQ(tif, nullptr);
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors()[0].message,
                HasSubstr("Cannot read TIFF header"));
  }

  // Write failure while writing initial header in "w" mode.
  {
    MemoryTiffStream stream;
    stream.FailWritesAfter(0);
    UniqueTiff tif = stream.Open("w");
    EXPECT_EQ(tif, nullptr);
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors()[0].message,
                HasSubstr("Error writing TIFF header"));
  }

  // Bad magic numbers (including MDI_BIGENDIAN on little-endian host).
  {
    std::vector<uint8_t> bad_magic = {'X', 'Y', 42, 0, 8, 0, 0, 0};
    MemoryTiffStream stream(bad_magic);
    EXPECT_EQ(stream.Open("r"), nullptr);
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors()[0].message,
                HasSubstr("bad magic number"));
  }
  {
    std::vector<uint8_t> mdi_be = {
        static_cast<uint8_t>(MDI_BIGENDIAN & 0xffU),
        static_cast<uint8_t>((MDI_BIGENDIAN >> 8) & 0xffU),
        42,
        0,
        8,
        0,
        0,
        0,
    };
    MemoryTiffStream stream(mdi_be);
    EXPECT_EQ(stream.Open("r"), nullptr);
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors()[0].message,
                HasSubstr("bad magic number"));
  }

  // Bad version numbers in LE and BE.
  for (bool be : {false, true}) {
    SCOPED_TRACE(be);
    std::vector<uint8_t> hdr =
        be ? std::vector<uint8_t>{'M', 'M', 0, 44, 0, 0, 0, 8}
           : std::vector<uint8_t>{'I', 'I', 44, 0, 8, 0, 0, 0};
    MemoryTiffStream stream(hdr);
    EXPECT_EQ(stream.Open("r"), nullptr);
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors()[0].message,
                HasSubstr("bad version number"));
  }

  // Truncated BigTIFF header (8 bytes instead of 16).
  {
    std::vector<uint8_t> trunc_big = {'I', 'I', 43, 0, 8, 0, 0, 0};
    MemoryTiffStream stream(trunc_big);
    EXPECT_EQ(stream.Open("r"), nullptr);
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors()[0].message,
                HasSubstr("Cannot read TIFF header"));
  }

  // Bad BigTIFF offsetsize (!= 8) in LE and BE.
  for (bool be : {false, true}) {
    SCOPED_TRACE(be);
    std::vector<uint8_t> bad_off =
        be ? std::vector<uint8_t>{'M', 'M', 0, 43, 0, 4, 0, 0,
                                  0,   0,   0, 0,  0, 0, 0, 16}
           : std::vector<uint8_t>{'I', 'I', 43, 0, 4, 0, 0, 0,
                                  16,  0,   0,  0, 0, 0, 0, 0};
    MemoryTiffStream stream(bad_off);
    EXPECT_EQ(stream.Open("r"), nullptr);
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors()[0].message,
                HasSubstr("bad BigTIFF offsetsize"));
  }

  // Bad BigTIFF unused field (!= 0) in LE and BE.
  for (bool be : {false, true}) {
    SCOPED_TRACE(be);
    std::vector<uint8_t> bad_unused =
        be ? std::vector<uint8_t>{'M', 'M', 0, 43, 0, 8, 0, 1,
                                  0,   0,   0, 0,  0, 0, 0, 16}
           : std::vector<uint8_t>{'I', 'I', 43, 0, 8, 0, 1, 0,
                                  16,  0,   0,  0, 0, 0, 0, 0};
    MemoryTiffStream stream(bad_unused);
    EXPECT_EQ(stream.Open("r"), nullptr);
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors()[0].message,
                HasSubstr("bad BigTIFF unused"));
  }

  // Read failure (ReadProc returning -1) on header read in "r" mode.
  {
    MemoryTiffStream stream(BuildMinimalStripTiff(false, false));
    stream.FailReadsAfter(0, -1);
    EXPECT_EQ(stream.Open("r"), nullptr);
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors()[0].message,
                HasSubstr("Cannot read TIFF header"));
  }

  // Valid ClassicTIFF and BigTIFF headers whose initial IFD fails to read.
  // tif_dirread.c emits "Invalid zero directory count" via TIFFErrorExt, so
  // install ScopedTiffErrorHandlers to capture it silently without stderr spam.
  {
    ScopedTiffErrorHandlers global_guard(&GlobalDiagnosticCapture::OnError,
                                         &GlobalDiagnosticCapture::OnWarning);
    for (bool big_tiff : {false, true}) {
      for (const char* mode : {"r", "r+"}) {
        SCOPED_TRACE(big_tiff);
        SCOPED_TRACE(mode);
        GlobalDiagnosticCapture::Clear();
        RawTiffBuilder builder(/*big_endian=*/false, big_tiff);
        builder.AddIfd({});  // dircount == 0 fails TIFFReadDirectory cleanly.
        MemoryTiffStream stream(builder.bytes());
        EXPECT_EQ(stream.Open(mode), nullptr);
        EXPECT_FALSE(GlobalDiagnosticCapture::errors().empty());
        EXPECT_THAT(GlobalDiagnosticCapture::errors()[0].message,
                    HasSubstr("Invalid zero directory count"));
      }
    }
  }
}

TEST(TifOpenTest, BigTiffReadBigAndLittleEndian) {
  for (bool be : {false, true}) {
    SCOPED_TRACE(be);
    MemoryTiffStream stream(BuildMinimalStripTiff(be, /*big_tiff=*/true));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFIsBigTIFF(tif.get()), 1);
    EXPECT_EQ(TIFFIsBigEndian(tif.get()), be ? 1 : 0);
    EXPECT_EQ(TIFFIsByteSwapped(tif.get()), be ? 1 : 0);
    EXPECT_EQ(TIFFIsTiled(tif.get()), 0);
    EXPECT_EQ(TIFFCurrentDirectory(tif.get()), 0U);
  }
}

TEST(TifOpenTest, HandleAccessorsAndMutators) {
  MemoryTiffStream stream(BuildMinimalStripTiff(/*big_endian=*/false,
                                                /*big_tiff=*/false));
  UniqueTiff tif = stream.Open("r", nullptr, "initial_name.tiff");
  ASSERT_NE(tif, nullptr);

  // TIFFFileName / TIFFSetFileName.
  EXPECT_EQ(std::string_view(TIFFFileName(tif.get())), "initial_name.tiff");
  const char* prev_name = TIFFSetFileName(tif.get(), "renamed.tiff");
  EXPECT_EQ(std::string_view(prev_name), "initial_name.tiff");
  EXPECT_EQ(std::string_view(TIFFFileName(tif.get())), "renamed.tiff");
  TIFFSetFileName(tif.get(), prev_name);
  EXPECT_EQ(std::string_view(TIFFFileName(tif.get())), "initial_name.tiff");

  // TIFFFileno / TIFFSetFileno.
  EXPECT_EQ(TIFFFileno(tif.get()), 0);
  EXPECT_EQ(TIFFSetFileno(tif.get(), 42), 0);
  EXPECT_EQ(TIFFFileno(tif.get()), 42);
  EXPECT_EQ(TIFFSetFileno(tif.get(), 0), 42);

  // TIFFClientdata / TIFFSetClientdata (restore before TIFFClose!).
  thandle_t orig_cd = TIFFClientdata(tif.get());
  EXPECT_EQ(orig_cd, static_cast<thandle_t>(&stream));
  int sentinel = 0;
  const auto restore_cd =
      absl::MakeCleanup([&] { TIFFSetClientdata(tif.get(), orig_cd); });
  EXPECT_EQ(TIFFSetClientdata(tif.get(), static_cast<thandle_t>(&sentinel)),
            orig_cd);
  EXPECT_EQ(TIFFClientdata(tif.get()), static_cast<thandle_t>(&sentinel));

  // TIFFGetMode / TIFFSetMode (restore O_RDONLY before TIFFClose!).
  const int orig_mode = TIFFGetMode(tif.get());
  EXPECT_EQ(orig_mode, O_RDONLY);
  const auto restore_mode =
      absl::MakeCleanup([&] { TIFFSetMode(tif.get(), orig_mode); });
  EXPECT_EQ(TIFFSetMode(tif.get(), O_RDWR), O_RDONLY);
  EXPECT_EQ(TIFFGetMode(tif.get()), O_RDWR);

  // Client callback proc getters.
  EXPECT_EQ(TIFFGetReadProc(tif.get()), &MemoryTiffStream::ReadProc);
  EXPECT_EQ(TIFFGetWriteProc(tif.get()), &MemoryTiffStream::WriteProc);
  EXPECT_EQ(TIFFGetSeekProc(tif.get()), &MemoryTiffStream::SeekProc);
  EXPECT_EQ(TIFFGetCloseProc(tif.get()), &MemoryTiffStream::CloseProc);
  EXPECT_EQ(TIFFGetSizeProc(tif.get()), &MemoryTiffStream::SizeProc);
  EXPECT_EQ(TIFFGetMapFileProc(tif.get()), &_tiffDummyMapProc);
  EXPECT_EQ(TIFFGetUnmapFileProc(tif.get()), &_tiffDummyUnmapProc);
}

TEST(TifOpenTest, CurrentRowStripTileDirectoryAndUpsampledQueries) {
  // Scanline / Strip progress tracking.
  {
    MemoryTiffStream stream(BuildMinimalStripTiff(false, false, 4, 3));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFIsTiled(tif.get()), 0);
    EXPECT_EQ(TIFFCurrentDirectory(tif.get()), 0U);
    EXPECT_EQ(TIFFCurrentDirOffset(tif.get()), 20U);
    EXPECT_EQ(TIFFIsUpSampled(tif.get()), 0);

    std::vector<uint8_t> row(4);
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), 1);
    EXPECT_EQ(TIFFCurrentRow(tif.get()), 1U);
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 1, 0), 1);
    EXPECT_EQ(TIFFCurrentRow(tif.get()), 2U);
    EXPECT_EQ(TIFFCurrentStrip(tif.get()), 0U);

    std::vector<uint8_t> strip(4 * 3);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, strip.data(),
                                   static_cast<tmsize_t>(strip.size())),
              static_cast<tmsize_t>(strip.size()));
    EXPECT_EQ(TIFFCurrentStrip(tif.get()), 0U);
  }

  // Multi-strip and multi-tile write progress tracking.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{2}), 1);

    std::vector<uint8_t> strip0(4 * 2, 0x10);
    std::vector<uint8_t> strip1(4 * 2, 0x20);
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, strip0.data(), 8), 8);
    EXPECT_EQ(TIFFCurrentStrip(tif.get()), 0U);
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 1, strip1.data(), 8), 8);
    EXPECT_EQ(TIFFCurrentStrip(tif.get()), 1U);
  }

  // Tiled progress tracking (pass size == -1 so TIFFFillTile/TIFFStartTile runs
  // and updates td_curtile rather than taking the uncompressed shortcut).
  {
    MemoryTiffStream stream(BuildMinimalTiledTiff(false, false));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFIsTiled(tif.get()), 1);

    std::vector<uint8_t> tile(16 * 16);
    EXPECT_EQ(TIFFReadEncodedTile(tif.get(), 0, tile.data(),
                                  static_cast<tmsize_t>(-1)),
              static_cast<tmsize_t>(tile.size()));
    EXPECT_EQ(TIFFCurrentTile(tif.get()), 0U);
    EXPECT_EQ(tile[0], 0x33);
  }

  // Multi-tile write advances TIFFCurrentTile from 0 to 1.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{32}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{16}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, uint32_t{16}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, uint32_t{16}), 1);

    std::vector<uint8_t> tile0(16 * 16, 0x41);
    std::vector<uint8_t> tile1(16 * 16, 0x42);
    EXPECT_EQ(TIFFWriteEncodedTile(tif.get(), 0, tile0.data(), 256), 256);
    EXPECT_EQ(TIFFCurrentTile(tif.get()), 0U);
    EXPECT_EQ(TIFFWriteEncodedTile(tif.get(), 1, tile1.data(), 256), 256);
    EXPECT_EQ(TIFFCurrentTile(tif.get()), 1U);
  }

  // JPEG YCbCr upsampling flag via JPEGCOLORMODE_RGB.
  if (TIFFIsCODECConfigured(COMPRESSION_JPEG)) {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
              1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
              1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB),
              1);
    EXPECT_EQ(TIFFIsUpSampled(tif.get()), 1);

    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RAW),
              1);
    EXPECT_EQ(TIFFIsUpSampled(tif.get()), 0);
  }
}

TEST(TifOpenTest, TempFileFdAndFileOpen) {
  TempFile temp("tif_open_test.tif");
  DiagnosticRecorder diag;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  diag.Install(opts.get());

  // Write via TIFFOpenExt.
  {
    UniqueTiff writer(TIFFOpenExt(temp.c_str(), "w", opts.get()));
    ASSERT_NE(writer, nullptr);
    EXPECT_GE(TIFFFileno(writer.get()), 0);
    EXPECT_EQ(std::string_view(TIFFFileName(writer.get())), temp.string());
    WriteSimpleStripDirectory(writer.get(), 4, 2, 0x99);
  }

  // Read via TIFFFdOpenExt with explicit fd ownership on error path.
  {
    const int fd = ::open(temp.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    UniqueTiff reader(TIFFFdOpenExt(fd, temp.c_str(), "rM", opts.get()));
    if (reader == nullptr) {
      ::close(fd);
      FAIL() << "TIFFFdOpenExt unexpectedly failed";
    }
    EXPECT_EQ(TIFFFileno(reader.get()), fd);
    EXPECT_NE(reader->tif_flags & TIFF_MAPPED, 0U);
    uint32_t width = 0;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGEWIDTH, &width), 1);
    EXPECT_EQ(width, 4U);
  }

  // TIFFFdOpenExt failure path on an empty file must close fd manually.
  {
    TempFile empty_file("empty_fd_open.tif");
    const int wfd =
        ::open(empty_file.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
    ASSERT_GE(wfd, 0);
    ::close(wfd);

    const int rfd = ::open(empty_file.c_str(), O_RDONLY);
    ASSERT_GE(rfd, 0);
    diag.Clear();
    UniqueTiff failed_fd(
        TIFFFdOpenExt(rfd, empty_file.c_str(), "r", opts.get()));
    if (failed_fd == nullptr) {
      EXPECT_EQ(::close(rfd), 0);
    }
    EXPECT_EQ(failed_fd, nullptr);
    ASSERT_FALSE(diag.errors().empty());
    EXPECT_THAT(diag.errors().back().message,
                HasSubstr("Cannot read TIFF header"));
  }

  // Missing file fails cleanly via TIFFOpenOptions handler (silent stderr).
  TempFile missing("does_not_exist.tif");
  UniqueTiff bad_open(TIFFOpenExt(missing.c_str(), "r", opts.get()));
  EXPECT_EQ(bad_open, nullptr);
  ASSERT_FALSE(diag.errors().empty());
  EXPECT_EQ(diag.errors().back().tif, nullptr);
  EXPECT_THAT(diag.errors().back().message, HasSubstr("does_not_exist.tif"));
}

}  // namespace
}  // namespace libtiff
