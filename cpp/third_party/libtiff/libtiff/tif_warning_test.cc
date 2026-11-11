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

// Unit tests for libtiff warning handler registration and dispatch
// (libtiff/tif_warning.c).
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_warning.c

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {
namespace {

enum class HandlerKind { kBasic, kExt, kExtR };

struct CapturedWarning {
  HandlerKind kind = HandlerKind::kBasic;
  TIFF* tif = nullptr;
  void* user_data = nullptr;
  thandle_t fd = nullptr;
  std::optional<std::string> module;
  std::string message;
};

struct ExtRContext {
  std::vector<CapturedWarning>* sink = nullptr;
  int return_value = 1;
};

std::vector<CapturedWarning>* g_warning_sink = nullptr;

// Binds g_warning_sink to an owned vector for the current test scope.
// Declare before ScopedTiffErrorHandlers* so handlers are restored before the
// sink pointer is cleared.
class ScopedWarningSink {
 public:
  ScopedWarningSink() { g_warning_sink = &records_; }
  ~ScopedWarningSink() { g_warning_sink = nullptr; }

  ScopedWarningSink(const ScopedWarningSink&) = delete;
  ScopedWarningSink& operator=(const ScopedWarningSink&) = delete;

  std::vector<CapturedWarning>& records() { return records_; }
  const std::vector<CapturedWarning>& records() const { return records_; }
  void Clear() { records_.clear(); }

 private:
  std::vector<CapturedWarning> records_;
};

std::string FormatWarningVa(const char* fmt, va_list ap) {
  if (fmt == nullptr) return {};
  va_list copy_len;
  va_copy(copy_len, ap);
  const int len = std::vsnprintf(nullptr, 0, fmt, copy_len);
  va_end(copy_len);
  if (len < 0) return {};

  std::string out(static_cast<std::size_t>(len) + 1, '\0');
  va_list copy_fmt;
  va_copy(copy_fmt, ap);
  std::vsnprintf(out.data(), out.size(), fmt, copy_fmt);
  va_end(copy_fmt);
  out.resize(static_cast<std::size_t>(len));
  return out;
}

std::optional<std::string> OptionalModule(const char* module) {
  if (module == nullptr) return std::nullopt;
  return std::string(module);
}

void CaptureBasicWarning(const char* module, const char* fmt, va_list ap) {
  if (g_warning_sink == nullptr) return;
  g_warning_sink->push_back({
      .kind = HandlerKind::kBasic,
      .module = OptionalModule(module),
      .message = FormatWarningVa(fmt, ap),
  });
}

void CaptureExtWarning(thandle_t fd, const char* module, const char* fmt,
                       va_list ap) {
  if (g_warning_sink == nullptr) return;
  g_warning_sink->push_back({
      .kind = HandlerKind::kExt,
      .fd = fd,
      .module = OptionalModule(module),
      .message = FormatWarningVa(fmt, ap),
  });
}

int CaptureExtRWarning(TIFF* tif, void* user_data, const char* module,
                       const char* fmt, va_list ap) {
  auto* ctx = static_cast<ExtRContext*>(user_data);
  std::vector<CapturedWarning>* sink =
      ctx != nullptr ? ctx->sink : g_warning_sink;
  if (sink != nullptr) {
    sink->push_back({
        .kind = HandlerKind::kExtR,
        .tif = tif,
        .user_data = user_data,
        .module = OptionalModule(module),
        .message = FormatWarningVa(fmt, ap),
    });
  }
  return ctx != nullptr ? ctx->return_value : 1;
}

std::vector<uint8_t> BuildMinimalTiffWithUnknownTag(bool big_endian,
                                                    bool big_tiff,
                                                    uint16_t unknown_tag) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const std::array<uint8_t, 1> pixel = {0x42};
  const uint64_t pixel_off = builder.AppendBlob(pixel);
  builder.AddIfd({
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, 1),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, 1),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(pixel_off)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
      builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 1),
      builder.MakeShort(unknown_tag, 123),
  });
  return builder.Take();
}

TEST(TifWarningTest, SetWarningHandlerReturnsPreviousAndRestores) {
  ScopedWarningSink sink;
  ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);
  TIFFErrorHandler prev_err = TIFFSetErrorHandler(nullptr);
  TIFFErrorHandler prev_warn = TIFFSetWarningHandler(&CaptureBasicWarning);
  absl::Cleanup restore = [prev_err, prev_warn] {
    TIFFSetErrorHandler(prev_err);
    TIFFSetWarningHandler(prev_warn);
  };

  EXPECT_NE(prev_warn, nullptr);
  EXPECT_EQ(TIFFSetWarningHandler(nullptr), &CaptureBasicWarning);
  EXPECT_EQ(TIFFSetWarningHandler(&CaptureBasicWarning), nullptr);
  EXPECT_TRUE(sink.records().empty());
}

TEST(TifWarningTest, SetWarningHandlerExtDefaultsToNullAndRestores) {
  ScopedWarningSink sink;
  ScopedTiffErrorHandlers basic_guard(nullptr, nullptr);
  TIFFErrorHandlerExt prev_err_ext = TIFFSetErrorHandlerExt(nullptr);
  TIFFErrorHandlerExt prev_warn_ext =
      TIFFSetWarningHandlerExt(&CaptureExtWarning);
  absl::Cleanup restore_ext = [prev_err_ext, prev_warn_ext] {
    TIFFSetErrorHandlerExt(prev_err_ext);
    TIFFSetWarningHandlerExt(prev_warn_ext);
  };

  EXPECT_EQ(prev_warn_ext, nullptr);
  EXPECT_EQ(TIFFSetWarningHandlerExt(nullptr), &CaptureExtWarning);
  EXPECT_EQ(TIFFSetWarningHandlerExt(&CaptureExtWarning), nullptr);
  EXPECT_TRUE(sink.records().empty());
}

TEST(TifWarningTest, WarningDispatchesToBothBasicAndExtHandlers) {
  ScopedWarningSink sink;
  ScopedTiffErrorHandlers basic_guard(nullptr, &CaptureBasicWarning);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, &CaptureExtWarning);

  TIFFWarning("ModA", "count=%d label=%s", 7, "alpha");

  ASSERT_EQ(sink.records().size(), 2u);
  EXPECT_EQ(sink.records()[0].kind, HandlerKind::kBasic);
  EXPECT_EQ(sink.records()[0].module, "ModA");
  EXPECT_EQ(sink.records()[0].message, "count=7 label=alpha");

  EXPECT_EQ(sink.records()[1].kind, HandlerKind::kExt);
  EXPECT_EQ(sink.records()[1].fd, nullptr);
  EXPECT_EQ(sink.records()[1].module, "ModA");
  EXPECT_EQ(sink.records()[1].message, "count=7 label=alpha");
}

TEST(TifWarningTest, WarningBasicOnlyDoesNotInvokeExtHandler) {
  ScopedWarningSink sink;
  ScopedTiffErrorHandlers basic_guard(nullptr, &CaptureBasicWarning);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);

  TIFFWarning(nullptr, "only basic %u", 42u);

  ASSERT_EQ(sink.records().size(), 1u);
  EXPECT_EQ(sink.records()[0].kind, HandlerKind::kBasic);
  EXPECT_EQ(sink.records()[0].module, std::nullopt);
  EXPECT_EQ(sink.records()[0].message, "only basic 42");
}

TEST(TifWarningTest, WarningExtOnlyDoesNotInvokeBasicHandler) {
  ScopedWarningSink sink;
  ScopedTiffErrorHandlers basic_guard(nullptr, nullptr);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, &CaptureExtWarning);

  TIFFWarning("OnlyExt", "msg=%s", "ok");

  ASSERT_EQ(sink.records().size(), 1u);
  EXPECT_EQ(sink.records()[0].kind, HandlerKind::kExt);
  EXPECT_EQ(sink.records()[0].fd, nullptr);
  EXPECT_EQ(sink.records()[0].module, "OnlyExt");
  EXPECT_EQ(sink.records()[0].message, "msg=ok");
}

TEST(TifWarningTest, WarningWithBothHandlersNullIsSafeNoOp) {
  ScopedWarningSink sink;
  ScopedTiffErrorHandlers basic_guard(nullptr, nullptr);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);

  TIFFWarning("Silent", "value=%d", 99);

  EXPECT_TRUE(sink.records().empty());
}

TEST(TifWarningTest, WarningExtPassesFdAndDispatchesToBothHandlers) {
  ScopedWarningSink sink;
  ScopedTiffErrorHandlers basic_guard(nullptr, &CaptureBasicWarning);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, &CaptureExtWarning);

  int client_context = 1234;
  const auto fd = static_cast<thandle_t>(&client_context);
  TIFFWarningExt(fd, "ExtMod", "x=%d y=%s", -3, "beta");

  ASSERT_EQ(sink.records().size(), 2u);
  EXPECT_EQ(sink.records()[0].kind, HandlerKind::kBasic);
  EXPECT_EQ(sink.records()[0].module, "ExtMod");
  EXPECT_EQ(sink.records()[0].message, "x=-3 y=beta");

  EXPECT_EQ(sink.records()[1].kind, HandlerKind::kExt);
  EXPECT_EQ(sink.records()[1].fd, fd);
  EXPECT_EQ(sink.records()[1].module, "ExtMod");
  EXPECT_EQ(sink.records()[1].message, "x=-3 y=beta");
}

TEST(TifWarningTest, WarningExtSingleAndNullHandlerCombinations) {
  ScopedWarningSink sink;
  ScopedTiffErrorHandlers basic_guard(nullptr, &CaptureBasicWarning);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);

  int client_context = 77;
  const auto fd = static_cast<thandle_t>(&client_context);
  TIFFWarningExt(fd, "BasicOnlyExt", "step=%d", 1);
  ASSERT_EQ(sink.records().size(), 1u);
  EXPECT_EQ(sink.records()[0].kind, HandlerKind::kBasic);
  EXPECT_EQ(sink.records()[0].module, "BasicOnlyExt");
  EXPECT_EQ(sink.records()[0].message, "step=1");

  sink.Clear();
  TIFFSetWarningHandler(nullptr);
  TIFFSetWarningHandlerExt(&CaptureExtWarning);
  TIFFWarningExt(fd, nullptr, "step=%d", 2);
  ASSERT_EQ(sink.records().size(), 1u);
  EXPECT_EQ(sink.records()[0].kind, HandlerKind::kExt);
  EXPECT_EQ(sink.records()[0].fd, fd);
  EXPECT_EQ(sink.records()[0].module, std::nullopt);
  EXPECT_EQ(sink.records()[0].message, "step=2");

  sink.Clear();
  TIFFSetWarningHandlerExt(nullptr);
  TIFFWarningExt(fd, "SilentExt", "step=%d", 3);
  EXPECT_TRUE(sink.records().empty());
}

TEST(TifWarningTest, WarningExtRStopsChainingWhenHandlerReturnsNonZero) {
  ScopedWarningSink sink;
  ScopedTiffErrorHandlers basic_guard(nullptr, &CaptureBasicWarning);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, &CaptureExtWarning);
  ExtRContext ctx{.sink = &sink.records(), .return_value = 1};
  MemoryTiffStream stream;

  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  TIFFOpenOptionsSetWarningHandlerExtR(opts.get(), &CaptureExtRWarning, &ctx);

  UniqueTiff tif = stream.Open("w", opts.get());
  ASSERT_NE(tif, nullptr);
  // Options are copied into TIFF* during open; verify freeing opts is safe.
  opts.reset();

  TIFFWarningExtR(tif.get(), "StopMod", "code=%d tag=%s", 5, "halt");
  ASSERT_EQ(sink.records().size(), 1u);
  EXPECT_EQ(sink.records()[0].kind, HandlerKind::kExtR);
  EXPECT_EQ(sink.records()[0].tif, tif.get());
  EXPECT_EQ(sink.records()[0].user_data, &ctx);
  EXPECT_EQ(sink.records()[0].module, "StopMod");
  EXPECT_EQ(sink.records()[0].message, "code=5 tag=halt");

  sink.Clear();
  ctx.return_value = 7;
  TIFFWarningExtR(tif.get(), "StopModNonOne", "val=%u", 9u);
  ASSERT_EQ(sink.records().size(), 1u);
  EXPECT_EQ(sink.records()[0].kind, HandlerKind::kExtR);
  EXPECT_EQ(sink.records()[0].module, "StopModNonOne");
  EXPECT_EQ(sink.records()[0].message, "val=9");
}

TEST(TifWarningTest, WarningExtRChainsToGlobalHandlersWhenHandlerReturnsZero) {
  ScopedWarningSink sink;
  ScopedTiffErrorHandlers basic_guard(nullptr, &CaptureBasicWarning);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, &CaptureExtWarning);
  ExtRContext ctx{.sink = &sink.records(), .return_value = 0};
  MemoryTiffStream stream;

  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  TIFFOpenOptionsSetWarningHandlerExtR(opts.get(), &CaptureExtRWarning, &ctx);

  UniqueTiff tif = stream.Open("w", opts.get());
  ASSERT_NE(tif, nullptr);

  TIFFWarningExtR(tif.get(), "ChainMod", "a=%d b=%s", 12, "next");

  ASSERT_EQ(sink.records().size(), 3u);
  EXPECT_EQ(sink.records()[0].kind, HandlerKind::kExtR);
  EXPECT_EQ(sink.records()[0].tif, tif.get());
  EXPECT_EQ(sink.records()[0].user_data, &ctx);
  EXPECT_EQ(sink.records()[0].module, "ChainMod");
  EXPECT_EQ(sink.records()[0].message, "a=12 b=next");

  EXPECT_EQ(sink.records()[1].kind, HandlerKind::kBasic);
  EXPECT_EQ(sink.records()[1].module, "ChainMod");
  EXPECT_EQ(sink.records()[1].message, "a=12 b=next");

  EXPECT_EQ(sink.records()[2].kind, HandlerKind::kExt);
  EXPECT_EQ(sink.records()[2].fd, static_cast<thandle_t>(&stream));
  EXPECT_EQ(sink.records()[2].module, "ChainMod");
  EXPECT_EQ(sink.records()[2].message, "a=12 b=next");
}

TEST(TifWarningTest,
     WarningExtRWithNullHandleSkipsExtRAndPassesZeroClientData) {
  ScopedWarningSink sink;
  ScopedTiffErrorHandlers basic_guard(nullptr, &CaptureBasicWarning);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, &CaptureExtWarning);

  TIFFWarningExtR(nullptr, "NullTif", "v=%u", 101u);

  ASSERT_EQ(sink.records().size(), 2u);
  EXPECT_EQ(sink.records()[0].kind, HandlerKind::kBasic);
  EXPECT_EQ(sink.records()[0].module, "NullTif");
  EXPECT_EQ(sink.records()[0].message, "v=101");

  EXPECT_EQ(sink.records()[1].kind, HandlerKind::kExt);
  EXPECT_EQ(sink.records()[1].fd, nullptr);
  EXPECT_EQ(sink.records()[1].module, "NullTif");
  EXPECT_EQ(sink.records()[1].message, "v=101");

  sink.Clear();
  TIFFSetWarningHandler(nullptr);
  TIFFSetWarningHandlerExt(nullptr);
  TIFFWarningExtR(nullptr, "NullTifSilent", "v=%u", 202u);
  EXPECT_TRUE(sink.records().empty());
}

TEST(TifWarningTest, WarningExtRWithoutPerHandleHandlerFallsBackToGlobal) {
  ScopedWarningSink sink;
  ScopedTiffErrorHandlers basic_guard(nullptr, &CaptureBasicWarning);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, &CaptureExtWarning);
  MemoryTiffStream stream;

  // Empty options leave tif_warnhandler == nullptr.
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  UniqueTiff tif = stream.Open("w", opts.get());
  ASSERT_NE(tif, nullptr);

  TIFFWarningExtR(tif.get(), nullptr, "fallback %d", 33);
  ASSERT_EQ(sink.records().size(), 2u);
  EXPECT_EQ(sink.records()[0].kind, HandlerKind::kBasic);
  EXPECT_EQ(sink.records()[0].module, std::nullopt);
  EXPECT_EQ(sink.records()[0].message, "fallback 33");
  EXPECT_EQ(sink.records()[1].kind, HandlerKind::kExt);
  EXPECT_EQ(sink.records()[1].fd, static_cast<thandle_t>(&stream));
  EXPECT_EQ(sink.records()[1].module, std::nullopt);
  EXPECT_EQ(sink.records()[1].message, "fallback 33");

  sink.Clear();
  TIFFSetWarningHandler(nullptr);
  TIFFWarningExtR(tif.get(), "ExtFallback", "only ext");
  ASSERT_EQ(sink.records().size(), 1u);
  EXPECT_EQ(sink.records()[0].kind, HandlerKind::kExt);
  EXPECT_EQ(sink.records()[0].fd, static_cast<thandle_t>(&stream));

  sink.Clear();
  TIFFSetWarningHandler(&CaptureBasicWarning);
  TIFFSetWarningHandlerExt(nullptr);
  TIFFWarningExtR(tif.get(), "BasicFallback", "only basic");
  ASSERT_EQ(sink.records().size(), 1u);
  EXPECT_EQ(sink.records()[0].kind, HandlerKind::kBasic);

  sink.Clear();
  TIFFSetWarningHandler(nullptr);
  TIFFWarningExtR(tif.get(), "NoHandlers", "silent");
  EXPECT_TRUE(sink.records().empty());
}

TEST(TifWarningTest, WarningExtRWithNullUserDataWorksCleanly) {
  ScopedWarningSink sink;
  ScopedTiffErrorHandlers basic_guard(nullptr, &CaptureBasicWarning);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, &CaptureExtWarning);
  MemoryTiffStream stream;

  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  TIFFOpenOptionsSetWarningHandlerExtR(opts.get(), &CaptureExtRWarning,
                                       nullptr);
  UniqueTiff tif = stream.Open("w", opts.get());
  ASSERT_NE(tif, nullptr);

  TIFFWarningExtR(tif.get(), "NullUser", "ok");

  ASSERT_EQ(sink.records().size(), 1u);
  EXPECT_EQ(sink.records()[0].kind, HandlerKind::kExtR);
  EXPECT_EQ(sink.records()[0].tif, tif.get());
  EXPECT_EQ(sink.records()[0].user_data, nullptr);
  EXPECT_EQ(sink.records()[0].module, "NullUser");
  EXPECT_EQ(sink.records()[0].message, "ok");
}

TEST(TifWarningTest,
     LibraryWarningsRouteThroughDiagnosticRecorderWithoutStderr) {
  DiagnosticRecorder recorder;
  MemoryTiffStream stream;

  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  recorder.Install(opts.get());

  // Mode 'H' is deprecated in tif_open.c and emits a warning via
  // TIFFWarningExtR during TIFFClientOpenExt.
  UniqueTiff tif = stream.Open("wH", opts.get());
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(recorder.warning_count(), 1u);
  EXPECT_EQ(recorder.warnings()[0].tif, tif.get());
  EXPECT_EQ(recorder.warnings()[0].module, "memory.tiff");
  EXPECT_NE(recorder.warnings()[0].message.find("H(ost) mode is deprecated"),
            std::string::npos);

  recorder.Clear();
  EXPECT_EQ(TIFFFieldWithTag(tif.get(), 0xdead), nullptr);
  ASSERT_EQ(recorder.warning_count(), 1u);
  EXPECT_EQ(recorder.warnings()[0].tif, tif.get());
  EXPECT_EQ(recorder.warnings()[0].module, "TIFFFieldWithTag");
  EXPECT_EQ(recorder.warnings()[0].message, "Warning, unknown tag 0xdead");

  recorder.Clear();
  EXPECT_EQ(TIFFFieldWithName(tif.get(), "NonExistentTiffTag"), nullptr);
  ASSERT_EQ(recorder.warning_count(), 1u);
  EXPECT_EQ(recorder.warnings()[0].tif, tif.get());
  EXPECT_EQ(recorder.warnings()[0].module, "TIFFFieldWithName");
  EXPECT_EQ(recorder.warnings()[0].message,
            "Warning, unknown tag NonExistentTiffTag");
  EXPECT_EQ(recorder.error_count(), 0u);
}

TEST(TifWarningTest, UnknownTagWarningRespectsOpenOptionsAcrossFormats) {
  struct FormatCase {
    const char* name;
    bool big_endian;
    bool big_tiff;
    int warn_unknown;
    std::size_t expected_warnings;
  };
  constexpr std::array<FormatCase, 4> kCases = {{
      {"ClassicLittleEndianWarn", false, false, 1, 1},
      {"ClassicBigEndianWarn", true, false, 1, 1},
      {"BigTiffLittleEndianWarn", false, true, 1, 1},
      {"ClassicLittleEndianSuppress", false, false, 0, 0},
  }};

  for (const FormatCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    DiagnosticRecorder recorder;
    MemoryTiffStream stream(BuildMinimalTiffWithUnknownTag(
        tc.big_endian, tc.big_tiff, /*unknown_tag=*/55555));
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    recorder.Install(opts.get());
    TIFFOpenOptionsSetWarnAboutUnknownTags(opts.get(), tc.warn_unknown);

    UniqueTiff tif = stream.Open("r", opts.get());
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(recorder.error_count(), 0u);
    ASSERT_EQ(recorder.warning_count(), tc.expected_warnings);
    if (tc.expected_warnings == 1u) {
      EXPECT_EQ(recorder.warnings()[0].tif, tif.get());
      EXPECT_NE(recorder.warnings()[0].message.find("55555"),
                std::string::npos);
    }
  }
}

}  // namespace
}  // namespace libtiff
