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

// Clean-room unit tests for third_party/tiff/tools/tiff_tools.c.

#include "third_party/tiff/tools/tiff_tools.h"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;

constexpr uint64_t kBytesPerMiB = 1024ULL * 1024ULL;
constexpr tmsize_t kSentinelLimit = -42;

// Captures STDERR_FILENO using pipe/dup2 while running `fn` and restores the
// original descriptor via absl::Cleanup before draining the pipe.
template <typename Fn>
std::string CaptureStderr(Fn&& fn) {
  std::fflush(stderr);
  const int saved_fd = ::dup(STDERR_FILENO);
  if (saved_fd < 0) {
    ADD_FAILURE() << "dup(STDERR_FILENO) failed with errno=" << errno;
    return {};
  }

  std::array<int, 2> pipe_fds = {-1, -1};
  if (::pipe(pipe_fds.data()) != 0) {
    ::close(saved_fd);
    ADD_FAILURE() << "pipe() failed with errno=" << errno;
    return {};
  }

  absl::Cleanup close_read = [&pipe_fds]() {
    if (pipe_fds[0] >= 0) {
      ::close(pipe_fds[0]);
      pipe_fds[0] = -1;
    }
  };

  if (::dup2(pipe_fds[1], STDERR_FILENO) < 0) {
    ::close(pipe_fds[1]);
    ::close(saved_fd);
    ADD_FAILURE() << "dup2() failed with errno=" << errno;
    return {};
  }
  ::close(pipe_fds[1]);
  pipe_fds[1] = -1;

  absl::Cleanup restore_stderr = [saved_fd]() {
    std::fflush(stderr);
    ::dup2(saved_fd, STDERR_FILENO);
    ::close(saved_fd);
  };

  std::forward<Fn>(fn)();
  std::move(restore_stderr).Invoke();

  std::string captured;
  std::array<char, 512> buf{};
  for (;;) {
    const ssize_t n = ::read(pipe_fds[0], buf.data(), buf.size());
    if (n > 0) {
      captured.append(buf.data(), static_cast<size_t>(n));
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  return captured;
}

TEST(TiffToolsTest, NullArgumentsReturnZeroAndPreserveLimit) {
  int res_null_arg = -1;
  int res_null_limit = -1;
  int res_both_null = -1;
  tmsize_t limit = kSentinelLimit;

  const std::string stderr_out = CaptureStderr([&]() {
    res_null_arg = TIFFToolsParseMemoryLimitMiB(nullptr, &limit);
    res_null_limit = TIFFToolsParseMemoryLimitMiB("16", nullptr);
    res_both_null = TIFFToolsParseMemoryLimitMiB(nullptr, nullptr);
  });

  EXPECT_EQ(res_null_arg, 0);
  EXPECT_EQ(res_null_limit, 0);
  EXPECT_EQ(res_both_null, 0);
  EXPECT_EQ(limit, kSentinelLimit);
  EXPECT_THAT(stderr_out, IsEmpty());
}

TEST(TiffToolsTest, ValidDecimalHexOctalAndWhitespaceInputs) {
  struct ValidCase {
    const char* input;
    tmsize_t expected_bytes;
  };
  constexpr auto kCases = std::to_array<ValidCase>({
      {.input = "0", .expected_bytes = 0},
      {.input = "+0", .expected_bytes = 0},
      {.input = "0   ", .expected_bytes = 0},
      {.input = "  00  ", .expected_bytes = 0},
      {.input = "1",
       .expected_bytes = static_cast<tmsize_t>(1ULL * kBytesPerMiB)},
      {.input = "16",
       .expected_bytes = static_cast<tmsize_t>(16ULL * kBytesPerMiB)},
      {.input = "256",
       .expected_bytes = static_cast<tmsize_t>(256ULL * kBytesPerMiB)},
      {.input = "4096",
       .expected_bytes = static_cast<tmsize_t>(4096ULL * kBytesPerMiB)},
      {.input = "+1",
       .expected_bytes = static_cast<tmsize_t>(1ULL * kBytesPerMiB)},
      {.input = "+0x10",
       .expected_bytes = static_cast<tmsize_t>(16ULL * kBytesPerMiB)},
      {.input = "0x0", .expected_bytes = 0},
      {.input = "0x10",
       .expected_bytes = static_cast<tmsize_t>(16ULL * kBytesPerMiB)},
      {.input = "0X1a",
       .expected_bytes = static_cast<tmsize_t>(26ULL * kBytesPerMiB)},
      {.input = "00", .expected_bytes = 0},
      {.input = "010",
       .expected_bytes = static_cast<tmsize_t>(8ULL * kBytesPerMiB)},
      {.input = "   42",
       .expected_bytes = static_cast<tmsize_t>(42ULL * kBytesPerMiB)},
      {.input = "\t\n\r\v\f 7",
       .expected_bytes = static_cast<tmsize_t>(7ULL * kBytesPerMiB)},
      {.input = "7 \t\n\r\v\f",
       .expected_bytes = static_cast<tmsize_t>(7ULL * kBytesPerMiB)},
      {.input = "  0x20  ",
       .expected_bytes = static_cast<tmsize_t>(32ULL * kBytesPerMiB)},
  });

  for (const ValidCase& tc : kCases) {
    SCOPED_TRACE(tc.input);
    tmsize_t limit = kSentinelLimit;
    int res = -1;
    const std::string stderr_out = CaptureStderr(
        [&]() { res = TIFFToolsParseMemoryLimitMiB(tc.input, &limit); });
    EXPECT_EQ(res, 1);
    EXPECT_EQ(limit, tc.expected_bytes);
    EXPECT_THAT(stderr_out, IsEmpty());
  }
}

TEST(TiffToolsTest, PreexistingErrnoDoesNotBreakValidParse) {
  tmsize_t limit = kSentinelLimit;
  int res = -1;
  const std::string stderr_out = CaptureStderr([&]() {
    errno = EINVAL;
    res = TIFFToolsParseMemoryLimitMiB("8", &limit);
  });

  EXPECT_EQ(res, 1);
  EXPECT_EQ(limit, static_cast<tmsize_t>(8ULL * kBytesPerMiB));
  EXPECT_THAT(stderr_out, IsEmpty());
}

TEST(TiffToolsTest, InvalidInputsReturnZeroAndPreserveLimit) {
  constexpr auto kInvalidInputs = std::to_array<const char*>({
      "",      "   ",     "\t\n\r\v\f", "-",       "  -\t", "-0",  "-1",
      "  -16", "\t-0x10", "- 10",       "+",       "++1",   "+-1", "+ 10",
      "abc",   "MiB",     "10MiB",      "10 MiB",  "10 20", "1.5", "1e3",
      "0x",    "0xG",     "08",         "123\x80", "\x80",
  });

  for (const char* input : kInvalidInputs) {
    SCOPED_TRACE(input);
    tmsize_t limit = kSentinelLimit;
    int res = -1;
    const std::string stderr_out = CaptureStderr(
        [&]() { res = TIFFToolsParseMemoryLimitMiB(input, &limit); });
    EXPECT_EQ(res, 0);
    EXPECT_EQ(limit, kSentinelLimit);
    EXPECT_THAT(stderr_out, IsEmpty());
  }
}

TEST(TiffToolsTest, StrtoullRangeOverflowReturnsZeroSilently) {
  const std::string umax_times_ten =
      absl::StrFormat("%u0", std::numeric_limits<uint64_t>::max());
  const std::vector<std::string> inputs = {
      "18446744073709551616", "999999999999999999999999999999",
      "0x10000000000000000",  "02000000000000000000000",
      umax_times_ten,
  };

  for (const std::string& input : inputs) {
    SCOPED_TRACE(input);
    tmsize_t limit = kSentinelLimit;
    int res = -1;
    const std::string stderr_out = CaptureStderr(
        [&]() { res = TIFFToolsParseMemoryLimitMiB(input.c_str(), &limit); });
    EXPECT_EQ(res, 0);
    EXPECT_EQ(limit, kSentinelLimit);
    EXPECT_THAT(stderr_out, IsEmpty());
  }
}

TEST(TiffToolsTest, ExactTmsizeBoundarySucceedsAndAboveTmsizeFails) {
  const uint64_t max_mib =
      static_cast<uint64_t>(TIFF_TMSIZE_T_MAX) / kBytesPerMiB;
  const tmsize_t expected_max_bytes =
      static_cast<tmsize_t>(max_mib * kBytesPerMiB);

  const std::vector<std::string> valid_boundary_inputs = {
      absl::StrFormat("%llu", static_cast<unsigned long long>(max_mib)),
      absl::StrFormat("0x%llx", static_cast<unsigned long long>(max_mib)),
      absl::StrFormat("0%llo", static_cast<unsigned long long>(max_mib)),
  };

  for (const std::string& input : valid_boundary_inputs) {
    SCOPED_TRACE(input);
    tmsize_t limit = kSentinelLimit;
    int res = -1;
    const std::string stderr_out = CaptureStderr(
        [&]() { res = TIFFToolsParseMemoryLimitMiB(input.c_str(), &limit); });
    EXPECT_EQ(res, 1);
    EXPECT_EQ(limit, expected_max_bytes);
    EXPECT_THAT(stderr_out, IsEmpty());
  }

  // Values in (TIFF_TMSIZE_T_MAX / 1 MiB, UINT64_MAX / 1 MiB] pass
  // _TIFFMultiply64 but fail _TIFFCastUInt64ToSSize(NULL, ...), which stays
  // quiet on stderr when tif is NULL.
  const uint64_t first_tmsize_overflow_mib = max_mib + 1ULL;
  const uint64_t max_u64_no_mul_overflow_mib =
      std::numeric_limits<uint64_t>::max() / kBytesPerMiB;
  ASSERT_LE(first_tmsize_overflow_mib, max_u64_no_mul_overflow_mib);

  const std::vector<std::string> cast_overflow_inputs = {
      absl::StrFormat(
          "%llu", static_cast<unsigned long long>(first_tmsize_overflow_mib)),
      absl::StrFormat(
          "0x%llx", static_cast<unsigned long long>(first_tmsize_overflow_mib)),
      absl::StrFormat(
          "%llu", static_cast<unsigned long long>(max_u64_no_mul_overflow_mib)),
      absl::StrFormat("0x%llx", static_cast<unsigned long long>(
                                    max_u64_no_mul_overflow_mib)),
  };

  for (const std::string& input : cast_overflow_inputs) {
    SCOPED_TRACE(input);
    tmsize_t limit = kSentinelLimit;
    int res = -1;
    const std::string stderr_out = CaptureStderr(
        [&]() { res = TIFFToolsParseMemoryLimitMiB(input.c_str(), &limit); });
    EXPECT_EQ(res, 0);
    EXPECT_EQ(limit, kSentinelLimit);
    EXPECT_THAT(stderr_out, IsEmpty());
  }
}

TEST(TiffToolsTest, Multiply64OverflowReturnsZeroAndReportsErrorToStderr) {
  const uint64_t first_mul_overflow_mib =
      (std::numeric_limits<uint64_t>::max() / kBytesPerMiB) + 1ULL;
  const std::vector<std::string> mul_overflow_inputs = {
      absl::StrFormat("%llu",
                      static_cast<unsigned long long>(first_mul_overflow_mib)),
      absl::StrFormat("0x%llx",
                      static_cast<unsigned long long>(first_mul_overflow_mib)),
      absl::StrFormat("%llu", static_cast<unsigned long long>(
                                  std::numeric_limits<uint64_t>::max())),
      absl::StrFormat("0x%llx", static_cast<unsigned long long>(
                                    std::numeric_limits<uint64_t>::max())),
  };

  for (const std::string& input : mul_overflow_inputs) {
    SCOPED_TRACE(input);
    tmsize_t limit = kSentinelLimit;
    int res = -1;
    const std::string stderr_out = CaptureStderr(
        [&]() { res = TIFFToolsParseMemoryLimitMiB(input.c_str(), &limit); });
    EXPECT_EQ(res, 0);
    EXPECT_EQ(limit, kSentinelLimit);
    EXPECT_THAT(stderr_out, HasSubstr("memory limit"));
    EXPECT_THAT(stderr_out, HasSubstr("Integer overflow"));
  }

  // With the global libtiff error handler suppressed, even _TIFFMultiply64
  // overflow produces no stderr output.
  ScopedTiffErrorHandlers suppress_handlers(nullptr, nullptr);
  tmsize_t limit = kSentinelLimit;
  int res = -1;
  const std::string stderr_out = CaptureStderr([&]() {
    res = TIFFToolsParseMemoryLimitMiB(mul_overflow_inputs.front().c_str(),
                                       &limit);
  });
  EXPECT_EQ(res, 0);
  EXPECT_EQ(limit, kSentinelLimit);
  EXPECT_THAT(stderr_out, IsEmpty());
}

TEST(TiffToolsTest, ParsedLimitIntegratesWithOpenOptionsOnMemoryTiff) {
  tmsize_t one_mib_limit = kSentinelLimit;
  ASSERT_EQ(TIFFToolsParseMemoryLimitMiB("1", &one_mib_limit), 1);
  ASSERT_EQ(one_mib_limit, static_cast<tmsize_t>(kBytesPerMiB));

  tmsize_t zero_limit = kSentinelLimit;
  ASSERT_EQ(TIFFToolsParseMemoryLimitMiB("0", &zero_limit), 1);
  ASSERT_EQ(zero_limit, 0);

  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  const std::array<uint8_t, 1> pixel = {0x5A};
  const uint64_t strip_offset = builder.AppendBlob(pixel);
  builder.AddIfd({
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, 1),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, 1),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                       static_cast<uint32_t>(strip_offset)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
      builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 1),
  });

  MemoryTiffStream stream(builder.Take());
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts.get(), nullptr);
  stream.diagnostics().Install(opts.get());
  TIFFOpenOptionsSetMaxSingleMemAlloc(opts.get(), one_mib_limit);
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), one_mib_limit);

  UniqueTiff tif = stream.Open("r", opts.get());
  ASSERT_NE(tif.get(), nullptr);

  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t bps = 0;
  uint16_t spp = 0;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &width), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGELENGTH, &height), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_BITSPERSAMPLE, &bps), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp), 1);
  EXPECT_EQ(width, 1U);
  EXPECT_EQ(height, 1U);
  EXPECT_EQ(bps, 8);
  EXPECT_EQ(spp, 1);

  std::array<uint8_t, 1> scanline = {0};
  EXPECT_EQ(TIFFReadScanline(tif.get(), scanline.data(), 0, 0), 1);
  EXPECT_EQ(scanline[0], 0x5A);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

}  // namespace
}  // namespace libtiff
