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

// Unit tests for libtiff tif_unix.c (UNIX file/fd I/O, mmap, memory, and
// default error/warning handlers).
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_unix.c

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/temp_file.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {
namespace {

// RAII wrapper for a POSIX file descriptor.
class ScopedFd {
 public:
  explicit ScopedFd(int fd = -1) : fd_(fd) {}
  ~ScopedFd() { reset(); }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  ScopedFd(ScopedFd&& other) noexcept : fd_(other.release()) {}
  ScopedFd& operator=(ScopedFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  int get() const { return fd_; }

  int release() {
    int f = fd_;
    fd_ = -1;
    return f;
  }

  void reset(int new_fd = -1) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = new_fd;
  }

 private:
  int fd_;
};

// Opens a TIFF handle from `fd`. Per tif_open.c ownership rules, `TIFFClose`
// closes `fd` if TIFFFdOpen* succeeds, whereas on failure `TIFFCleanup` does
// not invoke closeproc and `fd` remains owned by `ScopedFd`.
UniqueTiff FdOpenTakingOwnershipOnSuccess(ScopedFd& fd, const char* name,
                                          const char* mode,
                                          TIFFOpenOptions* opts = nullptr) {
  TIFF* tif = opts != nullptr ? TIFFFdOpenExt(fd.get(), name, mode, opts)
                              : TIFFFdOpen(fd.get(), name, mode);
  if (tif != nullptr) {
    fd.release();
  }
  return UniqueTiff(tif);
}

thandle_t FdToHandle(int fd) {
  // Match tif_unix.c's fd_as_handle_union_t representation; initialize to zero
  // first so upper bits are deterministic under MSan.
  thandle_t handle = nullptr;
  if (fd < 0) {
    const intptr_t neg = static_cast<intptr_t>(fd);
    std::memcpy(&handle, &neg, sizeof(handle));
  } else {
    std::memcpy(&handle, &fd, sizeof(fd));
  }
  return handle;
}

void QuietErrorHandler(const char* /*module*/, const char* /*fmt*/,
                       va_list /*ap*/) {}

void QuietWarningHandler(const char* /*module*/, const char* /*fmt*/,
                         va_list /*ap*/) {}

template <typename Fn>
std::string CaptureStderr(Fn&& fn) {
  std::fflush(stderr);
  std::array<int, 2> pipe_fds = {-1, -1};
  if (::pipe(pipe_fds.data()) != 0) {
    ADD_FAILURE() << "pipe() failed: errno=" << errno;
    return {};
  }
  ScopedFd read_fd(pipe_fds[0]);
  ScopedFd write_fd(pipe_fds[1]);

  ScopedFd saved_stderr(::dup(STDERR_FILENO));
  if (saved_stderr.get() < 0) {
    ADD_FAILURE() << "dup(STDERR_FILENO) failed: errno=" << errno;
    return {};
  }

  if (::dup2(write_fd.get(), STDERR_FILENO) < 0) {
    ADD_FAILURE() << "dup2(pipe_write, STDERR_FILENO) failed: errno=" << errno;
    return {};
  }
  // Close the duplicate write end so STDERR_FILENO is the sole writer.
  write_fd.reset();

  bool restored = false;
  auto restore_stderr = [&]() {
    if (!restored) {
      std::fflush(stderr);
      if (saved_stderr.get() >= 0) {
        ::dup2(saved_stderr.get(), STDERR_FILENO);
        saved_stderr.reset();
      }
      restored = true;
    }
  };
  absl::Cleanup guard(restore_stderr);

  fn();
  restore_stderr();

  std::string out;
  std::array<char, 512> buf{};
  for (;;) {
    const ssize_t n = ::read(read_fd.get(), buf.data(), buf.size());
    if (n > 0) {
      out.append(buf.data(), static_cast<size_t>(n));
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  return out;
}

std::vector<uint8_t> MakePixelRow(uint32_t width, uint16_t spp, uint8_t seed) {
  std::vector<uint8_t> row(static_cast<size_t>(width) * spp);
  for (size_t i = 0; i < row.size(); ++i) {
    row[i] = static_cast<uint8_t>((seed + i * 17U) & 0xffU);
  }
  return row;
}

void WriteStrippedRgbImage(TIFF* tif, uint32_t width, uint32_t height,
                           uint32_t rows_per_strip, uint8_t seed) {
  ASSERT_NE(tif, nullptr);
  constexpr uint16_t kBitsPerSample = 8;
  constexpr uint16_t kSamplesPerPixel = 3;
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, kBitsPerSample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, kSamplesPerPixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION,
                         static_cast<uint16_t>(COMPRESSION_NONE)),
            1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                         static_cast<uint16_t>(PHOTOMETRIC_RGB)),
            1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG,
                         static_cast<uint16_t>(PLANARCONFIG_CONTIG)),
            1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_XRESOLUTION, 72.0), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_YRESOLUTION, 72.0), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT,
                         static_cast<uint16_t>(RESUNIT_INCH)),
            1);

  for (uint32_t row = 0; row < height; ++row) {
    // Pass a mutable buffer because TIFFWriteScanline may mutate in place.
    std::vector<uint8_t> buf =
        MakePixelRow(width, kSamplesPerPixel, static_cast<uint8_t>(seed + row));
    ASSERT_EQ(TIFFWriteScanline(tif, buf.data(), row, 0), 1) << "row=" << row;
  }
}

void VerifyStrippedRgbImage(TIFF* tif, uint32_t expected_width,
                            uint32_t expected_height, uint8_t seed) {
  ASSERT_NE(tif, nullptr);
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t bps = 0;
  uint16_t spp = 0;
  uint16_t compression = 0;
  uint16_t planar = 0;
  float xres = 0.0f;

  ASSERT_EQ(TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width), 1);
  ASSERT_EQ(TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height), 1);
  ASSERT_EQ(TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bps), 1);
  ASSERT_EQ(TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp), 1);
  ASSERT_EQ(TIFFGetField(tif, TIFFTAG_COMPRESSION, &compression), 1);
  ASSERT_EQ(TIFFGetField(tif, TIFFTAG_PLANARCONFIG, &planar), 1);
  ASSERT_EQ(TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres), 1);

  EXPECT_EQ(width, expected_width);
  EXPECT_EQ(height, expected_height);
  EXPECT_EQ(bps, 8U);
  EXPECT_EQ(spp, 3U);
  EXPECT_EQ(compression, COMPRESSION_NONE);
  EXPECT_EQ(planar, PLANARCONFIG_CONTIG);
  EXPECT_FLOAT_EQ(xres, 72.0f);

  const tmsize_t scanline_bytes = TIFFScanlineSize(tif);
  ASSERT_EQ(scanline_bytes, static_cast<tmsize_t>(expected_width) * 3);
  std::vector<uint8_t> actual(static_cast<size_t>(scanline_bytes));
  for (uint32_t row = 0; row < expected_height; ++row) {
    SCOPED_TRACE(row);
    ASSERT_EQ(TIFFReadScanline(tif, actual.data(), row, 0), 1);
    const std::vector<uint8_t> expected =
        MakePixelRow(expected_width, 3, static_cast<uint8_t>(seed + row));
    EXPECT_EQ(actual, expected);
  }
}

bool WriteAllBytes(int fd, absl::Span<const uint8_t> data) {
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    written += static_cast<size_t>(n);
  }
  return true;
}

TEST(TifUnixTest, MallocZeroReturnsNullAndNonZeroAllocates) {
  UniqueTiffBuffer<void> zero_buf(_TIFFmalloc(0));
  EXPECT_EQ(zero_buf.get(), nullptr);

  constexpr tmsize_t kSize = 64;
  UniqueTiffBuffer<uint8_t> buf(static_cast<uint8_t*>(_TIFFmalloc(kSize)));
  ASSERT_NE(buf.get(), nullptr);
  _TIFFmemset(buf.get(), 0xA5, kSize);
  for (tmsize_t i = 0; i < kSize; ++i) {
    EXPECT_EQ(buf.get()[i], 0xA5U);
  }
}

TEST(TifUnixTest, CallocZeroReturnsNullAndNonZeroZeroInitializes) {
  EXPECT_EQ(UniqueTiffBuffer<void>(_TIFFcalloc(0, 16)).get(), nullptr);
  EXPECT_EQ(UniqueTiffBuffer<void>(_TIFFcalloc(16, 0)).get(), nullptr);
  EXPECT_EQ(UniqueTiffBuffer<void>(_TIFFcalloc(0, 0)).get(), nullptr);

  constexpr tmsize_t kCount = 8;
  constexpr tmsize_t kElemSize = 4;
  UniqueTiffBuffer<uint8_t> buf(
      static_cast<uint8_t*>(_TIFFcalloc(kCount, kElemSize)));
  ASSERT_NE(buf.get(), nullptr);
  for (tmsize_t i = 0; i < kCount * kElemSize; ++i) {
    EXPECT_EQ(buf.get()[i], 0U);
  }
}

TEST(TifUnixTest, ReallocNullAllocatesAndResizesPreservingPrefix) {
  UniqueTiffBuffer<uint8_t> buf(
      static_cast<uint8_t*>(_TIFFrealloc(nullptr, 16)));
  ASSERT_NE(buf.get(), nullptr);
  for (uint8_t i = 0; i < 16; ++i) {
    buf.get()[i] = static_cast<uint8_t>(i + 1U);
  }

  void* grown = _TIFFrealloc(buf.get(), 64);
  ASSERT_NE(grown, nullptr);
  buf.release();
  buf.reset(static_cast<uint8_t*>(grown));
  for (uint8_t i = 0; i < 16; ++i) {
    EXPECT_EQ(buf.get()[i], static_cast<uint8_t>(i + 1U));
  }
  _TIFFmemset(buf.get() + 16, 0x7E, 48);

  void* shrunk = _TIFFrealloc(buf.get(), 8);
  ASSERT_NE(shrunk, nullptr);
  buf.release();
  buf.reset(static_cast<uint8_t*>(shrunk));
  for (uint8_t i = 0; i < 8; ++i) {
    EXPECT_EQ(buf.get()[i], static_cast<uint8_t>(i + 1U));
  }
}

TEST(TifUnixTest, FreeNullIsSafeNoOp) { _TIFFfree(nullptr); }

TEST(TifUnixTest, MemsetMemcpyMemcmpOperateCorrectly) {
  std::array<uint8_t, 16> a{};
  std::array<uint8_t, 16> b{};

  _TIFFmemset(a.data(), 0x3C, a.size());
  for (uint8_t byte : a) {
    EXPECT_EQ(byte, 0x3CU);
  }
  _TIFFmemset(a.data(), 0xFF, 0);
  EXPECT_EQ(a[0], 0x3CU);

  _TIFFmemcpy(b.data(), a.data(), a.size());
  EXPECT_EQ(_TIFFmemcmp(a.data(), b.data(), a.size()), 0);
  EXPECT_EQ(_TIFFmemcmp(a.data(), b.data(), 0), 0);

  b[7] = 0x3D;
  EXPECT_LT(_TIFFmemcmp(a.data(), b.data(), a.size()), 0);
  EXPECT_GT(_TIFFmemcmp(b.data(), a.data(), a.size()), 0);
}

TEST(TifUnixTest, DefaultUnixWarningAndErrorHandlersFormatStderr) {
  const std::string warn_with_mod =
      CaptureStderr([]() { TIFFWarning("UnixMod", "bad value %d", 42); });
  EXPECT_EQ(warn_with_mod, "UnixMod: Warning, bad value 42.\n");

  const std::string warn_no_mod =
      CaptureStderr([]() { TIFFWarning(nullptr, "no module %s", "ok"); });
  EXPECT_EQ(warn_no_mod, "Warning, no module ok.\n");

  const std::string err_with_mod =
      CaptureStderr([]() { TIFFError("UnixErr", "failed code %d", -7); });
  EXPECT_EQ(err_with_mod, "UnixErr: failed code -7.\n");

  const std::string err_no_mod =
      CaptureStderr([]() { TIFFError(nullptr, "plain error %u", 99U); });
  EXPECT_EQ(err_no_mod, "plain error 99.\n");
}

TEST(TifUnixTest, TiffOpenWriteAndReadRoundTripClassicLittleEndian) {
  TempFile file("tif_unix_open_le.tif");
  {
    UniqueTiff writer(TIFFOpen(file.c_str(), "wl"));
    ASSERT_NE(writer.get(), nullptr);
    EXPECT_EQ(TIFFIsBigEndian(writer.get()), 0);
    EXPECT_EQ(TIFFIsBigTIFF(writer.get()), 0);
    WriteStrippedRgbImage(writer.get(), 12, 8, 4, 5);
  }

  // Default "r" on a regular file uses mmap (_tiffMapProc / _tiffUnmapProc).
  UniqueTiff reader(TIFFOpen(file.c_str(), "r"));
  ASSERT_NE(reader.get(), nullptr);
  EXPECT_EQ(std::string_view(TIFFFileName(reader.get())), file.string());
  EXPECT_EQ(TIFFGetMode(reader.get()), O_RDONLY);
  EXPECT_GE(TIFFFileno(reader.get()), 0);
  EXPECT_EQ(TIFFIsBigEndian(reader.get()), 0);
  EXPECT_EQ(TIFFIsBigTIFF(reader.get()), 0);
  EXPECT_EQ(TIFFIsByteSwapped(reader.get()), 0);
  VerifyStrippedRgbImage(reader.get(), 12, 8, 5);
}

TEST(TifUnixTest, TiffOpenBigEndianAndBigTiffModes) {
  struct ModeCase {
    std::string_view filename;
    std::string_view write_mode;
    std::string_view read_mode;
    int expect_big_endian;
    int expect_big_tiff;
    int expect_swapped;
  };
  constexpr std::array<ModeCase, 3> kCases = {{
      {"tif_unix_classic_be.tif", "wb", "rm", 1, 0, 1},
      {"tif_unix_bigtiff_le.tif", "w8l", "r", 0, 1, 0},
      {"tif_unix_bigtiff_be.tif", "w8b", "rm", 1, 1, 1},
  }};

  for (size_t i = 0; i < kCases.size(); ++i) {
    const ModeCase& tc = kCases[i];
    SCOPED_TRACE(tc.write_mode);
    TempFile file(tc.filename);
    const std::string write_mode_str(tc.write_mode);
    const std::string read_mode_str(tc.read_mode);
    {
      UniqueTiff writer(TIFFOpen(file.c_str(), write_mode_str.c_str()));
      ASSERT_NE(writer.get(), nullptr);
      WriteStrippedRgbImage(writer.get(), 8, 6, 3,
                            static_cast<uint8_t>(10 + i));
    }

    UniqueTiff reader(TIFFOpen(file.c_str(), read_mode_str.c_str()));
    ASSERT_NE(reader.get(), nullptr);
    EXPECT_EQ(TIFFIsBigEndian(reader.get()), tc.expect_big_endian);
    EXPECT_EQ(TIFFIsBigTIFF(reader.get()), tc.expect_big_tiff);
    EXPECT_EQ(TIFFIsByteSwapped(reader.get()), tc.expect_swapped);
    VerifyStrippedRgbImage(reader.get(), 8, 6, static_cast<uint8_t>(10 + i));
  }
}

TEST(TifUnixTest, TiffOpenAppendAndReadWriteModes) {
  TempFile file("tif_unix_append.tif");
  {
    UniqueTiff writer(TIFFOpen(file.c_str(), "w"));
    ASSERT_NE(writer.get(), nullptr);
    WriteStrippedRgbImage(writer.get(), 6, 4, 2, 1);
  }
  {
    UniqueTiff appender(TIFFOpen(file.c_str(), "a"));
    ASSERT_NE(appender.get(), nullptr);
    WriteStrippedRgbImage(appender.get(), 6, 4, 2, 20);
    ASSERT_EQ(TIFFWriteDirectory(appender.get()), 1);
  }
  {
    UniqueTiff rw(TIFFOpen(file.c_str(), "r+"));
    ASSERT_NE(rw.get(), nullptr);
    EXPECT_EQ(TIFFNumberOfDirectories(rw.get()), 2U);
    ASSERT_EQ(TIFFSetDirectory(rw.get(), 1), 1);
    VerifyStrippedRgbImage(rw.get(), 6, 4, 20);

    ASSERT_EQ(TIFFCreateDirectory(rw.get()), 0);
    WriteStrippedRgbImage(rw.get(), 6, 4, 2, 40);
    ASSERT_EQ(TIFFWriteDirectory(rw.get()), 1);
  }

  UniqueTiff reader(TIFFOpen(file.c_str(), "r"));
  ASSERT_NE(reader.get(), nullptr);
  EXPECT_EQ(TIFFNumberOfDirectories(reader.get()), 3U);
  VerifyStrippedRgbImage(reader.get(), 6, 4, 1);
  ASSERT_EQ(TIFFSetDirectory(reader.get(), 1), 1);
  VerifyStrippedRgbImage(reader.get(), 6, 4, 20);
  ASSERT_EQ(TIFFSetDirectory(reader.get(), 2), 1);
  VerifyStrippedRgbImage(reader.get(), 6, 4, 40);
}

TEST(TifUnixTest, TiffOpenExtErrorPathsAreReportedWithoutStderrSpam) {
  DiagnosticRecorder recorder;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts.get(), nullptr);
  recorder.Install(opts.get());

  TempFile missing("tif_unix_missing.tif");
  EXPECT_EQ(UniqueTiff(TIFFOpenExt(missing.c_str(), "r", opts.get())).get(),
            nullptr);
  EXPECT_GE(recorder.error_count(), 1U);

  recorder.Clear();
  EXPECT_EQ(UniqueTiff(TIFFOpenExt(missing.c_str(), "invalid_mode", opts.get()))
                .get(),
            nullptr);
  EXPECT_GE(recorder.error_count(), 1U);

  recorder.Clear();
  EXPECT_EQ(UniqueTiff(TIFFOpenExt(missing.c_str(), "", opts.get())).get(),
            nullptr);
  EXPECT_GE(recorder.error_count(), 1U);

  TempDir dir("tif_unix_dir_not_file");
  recorder.Clear();
  EXPECT_EQ(UniqueTiff(TIFFOpenExt(dir.c_str(), "w", opts.get())).get(),
            nullptr);
  EXPECT_GE(recorder.error_count(), 1U);

  // Empty file: open() succeeds, TIFFFdOpenExt fails, TIFFOpenExt closes fd.
  TempFile empty("tif_unix_empty.tif");
  {
    ScopedFd fd(::open(empty.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666));
    ASSERT_GE(fd.get(), 0);
  }
  recorder.Clear();
  EXPECT_EQ(UniqueTiff(TIFFOpenExt(empty.c_str(), "r", opts.get())).get(),
            nullptr);
  EXPECT_GE(recorder.error_count(), 1U);

  // Truncated 4-byte file exercises short read + EOF in _tiffReadProc.
  TempFile truncated("tif_unix_truncated.tif");
  {
    ScopedFd fd(::open(truncated.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666));
    ASSERT_GE(fd.get(), 0);
    constexpr std::array<uint8_t, 4> kPrefix = {'I', 'I', 42, 0};
    ASSERT_TRUE(WriteAllBytes(fd.get(), kPrefix));
  }
  recorder.Clear();
  EXPECT_EQ(UniqueTiff(TIFFOpenExt(truncated.c_str(), "r", opts.get())).get(),
            nullptr);
  EXPECT_GE(recorder.error_count(), 1U);

  // Non-TIFF file with non-Ext TIFFOpen under scoped global quiet handlers.
  TempFile not_tiff("tif_unix_bad_magic.tif");
  {
    ScopedFd fd(::open(not_tiff.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666));
    ASSERT_GE(fd.get(), 0);
    constexpr std::array<uint8_t, 8> kBadMagic = {'N', 'O', 'T', 'T',
                                                  'I', 'F', 'F', '!'};
    ASSERT_TRUE(WriteAllBytes(fd.get(), kBadMagic));
  }
  ScopedTiffErrorHandlers quiet(&QuietErrorHandler, &QuietWarningHandler);
  EXPECT_EQ(UniqueTiff(TIFFOpen(not_tiff.c_str(), "r")).get(), nullptr);
  EXPECT_EQ(UniqueTiff(TIFFOpen(missing.c_str(), "r")).get(), nullptr);
  EXPECT_EQ(UniqueTiff(TIFFOpen(missing.c_str(), "bad_mode")).get(), nullptr);
}

TEST(TifUnixTest, TiffFdOpenWriteAndReadRoundTrip) {
  TempFile file("tif_unix_fd_roundtrip.tif");
  {
    ScopedFd wfd(::open(file.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666));
    ASSERT_GE(wfd.get(), 0);
    const int raw_wfd = wfd.get();

    UniqueTiff writer = FdOpenTakingOwnershipOnSuccess(wfd, file.c_str(), "w");
    ASSERT_NE(writer.get(), nullptr);
    EXPECT_EQ(wfd.get(), -1);
    EXPECT_EQ(TIFFFileno(writer.get()), raw_wfd);
    WriteStrippedRgbImage(writer.get(), 10, 8, 4, 33);
  }

  ScopedFd rfd(::open(file.c_str(), O_RDONLY));
  ASSERT_GE(rfd.get(), 0);
  const int raw_rfd = rfd.get();
  UniqueTiff reader = FdOpenTakingOwnershipOnSuccess(rfd, file.c_str(), "rm");
  ASSERT_NE(reader.get(), nullptr);
  EXPECT_EQ(rfd.get(), -1);
  EXPECT_EQ(TIFFFileno(reader.get()), raw_rfd);
  VerifyStrippedRgbImage(reader.get(), 10, 8, 33);
}

TEST(TifUnixTest, TiffFdOpenExtErrorPathsPreserveFdOwnership) {
  DiagnosticRecorder recorder;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts.get(), nullptr);
  recorder.Install(opts.get());

  // Bad mode: TIFFFdOpenExt returns nullptr and does NOT close fd.
  TempFile file("tif_unix_fd_badmode.tif");
  {
    ScopedFd fd(::open(file.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666));
    ASSERT_GE(fd.get(), 0);
    UniqueTiff tif =
        FdOpenTakingOwnershipOnSuccess(fd, file.c_str(), "z", opts.get());
    EXPECT_EQ(tif.get(), nullptr);
    EXPECT_GE(fd.get(), 0);
    EXPECT_EQ(::lseek(fd.get(), 0, SEEK_SET), 0);
  }

  // Negative fd in read mode triggers read(-1) < 0 in _tiffReadProc.
  recorder.Clear();
  EXPECT_EQ(
      UniqueTiff(TIFFFdOpenExt(-1, "negfd_read.tif", "r", opts.get())).get(),
      nullptr);
  EXPECT_GE(recorder.error_count(), 1U);

  // Read-only fd opened in write mode triggers write() < 0 in _tiffWriteProc.
  {
    ScopedFd ro_fd(::open(file.c_str(), O_RDONLY));
    ASSERT_GE(ro_fd.get(), 0);
    recorder.Clear();
    UniqueTiff tif =
        FdOpenTakingOwnershipOnSuccess(ro_fd, file.c_str(), "w", opts.get());
    EXPECT_EQ(tif.get(), nullptr);
    EXPECT_GE(ro_fd.get(), 0);
    EXPECT_GE(recorder.error_count(), 1U);
  }

  // Write-only fd opened in read mode triggers read() < 0 in _tiffReadProc.
  {
    ScopedFd wo_fd(::open(file.c_str(), O_WRONLY));
    ASSERT_GE(wo_fd.get(), 0);
    recorder.Clear();
    UniqueTiff tif =
        FdOpenTakingOwnershipOnSuccess(wo_fd, file.c_str(), "r", opts.get());
    EXPECT_EQ(tif.get(), nullptr);
    EXPECT_GE(wo_fd.get(), 0);
    EXPECT_GE(recorder.error_count(), 1U);
  }
}

TEST(TifUnixTest, PipeStreamTriggersSizeZeroMapFallbackAndPartialRead) {
  DiagnosticRecorder recorder;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts.get(), nullptr);
  recorder.Install(opts.get());

  // Header-only ("rh") open on a pipe: ReadOK succeeds, then _tiffMapProc sees
  // fstat st_size == 0 and returns 0 so libtiff falls back to unmapped I/O.
  {
    std::array<int, 2> pipe_fds = {-1, -1};
    ASSERT_EQ(::pipe(pipe_fds.data()), 0);
    ScopedFd pipe_r(pipe_fds[0]);
    ScopedFd pipe_w(pipe_fds[1]);

    RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
    ASSERT_TRUE(WriteAllBytes(pipe_w.get(), builder.bytes()));
    pipe_w.reset();

    UniqueTiff tif = FdOpenTakingOwnershipOnSuccess(pipe_r, "pipe_header.tif",
                                                    "rh", opts.get());
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(pipe_r.get(), -1);
    EXPECT_EQ(TIFFIsBigTIFF(tif.get()), 0);
    EXPECT_EQ(TIFFIsBigEndian(tif.get()), 0);
  }

  // Partial 4-byte pipe payload exercises multi-read loop + EOF (count == 0).
  {
    std::array<int, 2> pipe_fds = {-1, -1};
    ASSERT_EQ(::pipe(pipe_fds.data()), 0);
    ScopedFd pipe_r(pipe_fds[0]);
    ScopedFd pipe_w(pipe_fds[1]);

    constexpr std::array<uint8_t, 4> kHalfHeader = {'I', 'I', 42, 0};
    ASSERT_TRUE(WriteAllBytes(pipe_w.get(), kHalfHeader));
    pipe_w.reset();

    recorder.Clear();
    UniqueTiff tif = FdOpenTakingOwnershipOnSuccess(pipe_r, "pipe_short.tif",
                                                    "r", opts.get());
    EXPECT_EQ(tif.get(), nullptr);
    EXPECT_GE(pipe_r.get(), 0);
    EXPECT_GE(recorder.error_count(), 1U);
  }
}

TEST(TifUnixTest, DirectProcErrorPathsSizeSeekMapAndZeroByteIo) {
  TempFile file("tif_unix_proc_errors.tif");
  {
    UniqueTiff writer(TIFFOpen(file.c_str(), "w"));
    ASSERT_NE(writer.get(), nullptr);
    WriteStrippedRgbImage(writer.get(), 4, 4, 4, 9);
  }

  UniqueTiff reader(TIFFOpen(file.c_str(), "r"));
  ASSERT_NE(reader.get(), nullptr);

  const TIFFReadWriteProc read_proc = TIFFGetReadProc(reader.get());
  const TIFFReadWriteProc write_proc = TIFFGetWriteProc(reader.get());
  const TIFFSeekProc seek_proc = TIFFGetSeekProc(reader.get());
  const TIFFSizeProc size_proc = TIFFGetSizeProc(reader.get());
  const TIFFMapFileProc map_proc = TIFFGetMapFileProc(reader.get());

  ASSERT_NE(read_proc, nullptr);
  ASSERT_NE(write_proc, nullptr);
  ASSERT_NE(seek_proc, nullptr);
  ASSERT_NE(size_proc, nullptr);
  ASSERT_NE(map_proc, nullptr);

  const thandle_t bad_handle = FdToHandle(-1);
  EXPECT_EQ(size_proc(bad_handle), 0U);
  EXPECT_EQ(seek_proc(bad_handle, 0, SEEK_SET), static_cast<uint64_t>(-1));

  std::array<uint8_t, 4> scratch{};
  EXPECT_EQ(read_proc(TIFFClientdata(reader.get()), scratch.data(), 0),
            static_cast<tmsize_t>(-1));
  EXPECT_EQ(write_proc(TIFFClientdata(reader.get()), scratch.data(), 0),
            static_cast<tmsize_t>(-1));

  // Regular file opened O_WRONLY has non-zero fstat size, so mmap(PROT_READ)
  // fails with MAP_FAILED and _tiffMapProc returns 0.
  ScopedFd wronly_fd(::open(file.c_str(), O_WRONLY));
  ASSERT_GE(wronly_fd.get(), 0);
  void* base = nullptr;
  toff_t mapped_size = 0;
  EXPECT_EQ(map_proc(FdToHandle(wronly_fd.get()), &base, &mapped_size), 0);

  // Non-blocking pipe full write exercises partial write + loop increment +
  // EAGAIN error return.
  std::array<int, 2> pipe_fds = {-1, -1};
  ASSERT_EQ(::pipe(pipe_fds.data()), 0);
  ScopedFd pipe_r(pipe_fds[0]);
  ScopedFd pipe_w(pipe_fds[1]);
  const int flags = ::fcntl(pipe_w.get(), F_GETFL, 0);
  ASSERT_NE(flags, -1);
  ASSERT_EQ(::fcntl(pipe_w.get(), F_SETFL, flags | O_NONBLOCK), 0);
  std::vector<uint8_t> big_buf(256 * 1024, 0x55);
  EXPECT_EQ(write_proc(FdToHandle(pipe_w.get()), big_buf.data(),
                       static_cast<tmsize_t>(big_buf.size())),
            static_cast<tmsize_t>(-1));
}

TEST(TifUnixTest, AllocLimitOptionsAndGuardsWithTiffOpenExt) {
  DiagnosticRecorder recorder;
  TempFile file("tif_unix_alloc_limits.tif");
  {
    UniqueTiff writer(TIFFOpen(file.c_str(), "w"));
    ASSERT_NE(writer.get(), nullptr);
    WriteStrippedRgbImage(writer.get(), 16, 16, 16, 77);
  }

  // Early single-alloc limit rejects TIFFOpenExt cleanly.
  {
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts.get(), nullptr);
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxSingleMemAlloc(opts.get(), 16);
    EXPECT_EQ(UniqueTiff(TIFFOpenExt(file.c_str(), "r", opts.get())).get(),
              nullptr);
    EXPECT_GE(recorder.error_count(), 1U);
  }

  // Early cumulated-alloc limit rejects TIFFOpenExt cleanly.
  {
    recorder.Clear();
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts.get(), nullptr);
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 16);
    EXPECT_EQ(UniqueTiff(TIFFOpenExt(file.c_str(), "r", opts.get())).get(),
              nullptr);
    EXPECT_GE(recorder.error_count(), 1U);
  }

  // Open with non-zero cumulated budget, then tighten dynamically via guards.
  recorder.Clear();
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts.get(), nullptr);
  recorder.Install(opts.get());
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

  UniqueTiff reader(TIFFOpenExt(file.c_str(), "rm", opts.get()));
  ASSERT_NE(reader.get(), nullptr);

  const tmsize_t scanline_bytes = TIFFScanlineSize(reader.get());
  ASSERT_GT(scanline_bytes, 16);
  std::vector<uint8_t> row_buf(static_cast<size_t>(scanline_bytes));

  // Unmapped TIFFReadScanline allocates raw strip buffer via _TIFFmallocExt.
  {
    ScopedSingleMemAllocLimit single_guard(reader.get(), 8);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 0, 0), -1);
  }

  {
    ScopedCumulatedMemAllocRemaining cum_guard(reader.get(), 8);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 0, 0), -1);
  }

  // After guards exit, reading the scanlines succeeds normally.
  VerifyStrippedRgbImage(reader.get(), 16, 16, 77);
}

TEST(TifUnixTest, NonZeroSeekWorksAndReadsDirectory) {
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  const std::vector<uint8_t> strip = {10, 20, 30, 40, 50, 60};
  const uint64_t strip_off = builder.AppendBlob(strip);
  builder.AddIfd({
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, 2),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, 1),
      builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE,
                             std::array<uint16_t, 3>{8, 8, 8}),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
      builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(strip_off)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
      builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                       static_cast<uint32_t>(strip.size())),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
  });

  TempFile file("tif_unix_synthetic.tif");
  {
    ScopedFd fd(::open(file.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666));
    ASSERT_GE(fd.get(), 0);
    ASSERT_TRUE(WriteAllBytes(fd.get(), builder.bytes()));
  }

  UniqueTiff reader(TIFFOpen(file.c_str(), "rm"));
  ASSERT_NE(reader.get(), nullptr);
  std::array<uint8_t, 6> out{};
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), out.size()), 6);
  EXPECT_EQ(std::vector<uint8_t>(out.begin(), out.end()), strip);
}

TEST(TifUnixTest, TiffOpen16BitPlanarSeparateAndCompressedRoundTrip) {
  ASSERT_EQ(TIFFIsCODECConfigured(COMPRESSION_LZW), 1);
  constexpr uint32_t kWidth = 8;
  constexpr uint32_t kHeight = 4;
  constexpr uint16_t kBitsPerSample = 16;
  constexpr uint16_t kSamplesPerPixel = 3;

  TempFile file("tif_unix_16bit_separate_lzw.tif");
  {
    UniqueTiff writer(TIFFOpen(file.c_str(), "wb"));
    ASSERT_NE(writer.get(), nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kWidth), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kHeight), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, kBitsPerSample),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, kSamplesPerPixel),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kHeight), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                           static_cast<uint16_t>(COMPRESSION_LZW)),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                           static_cast<uint16_t>(PHOTOMETRIC_RGB)),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                           static_cast<uint16_t>(PLANARCONFIG_SEPARATE)),
              1);

    for (uint16_t sample = 0; sample < kSamplesPerPixel; ++sample) {
      for (uint32_t row = 0; row < kHeight; ++row) {
        std::vector<uint16_t> buf(kWidth);
        for (uint32_t col = 0; col < kWidth; ++col) {
          buf[col] =
              static_cast<uint16_t>(0x1000U * (sample + 1U) + row * 32U + col);
        }
        // Pass mutable buffer because big-endian write swabs in place.
        ASSERT_EQ(TIFFWriteScanline(writer.get(), buf.data(), row, sample), 1);
      }
    }
  }

  UniqueTiff reader(TIFFOpen(file.c_str(), "r"));
  ASSERT_NE(reader.get(), nullptr);
  EXPECT_EQ(TIFFIsBigEndian(reader.get()), 1);
  EXPECT_EQ(TIFFIsByteSwapped(reader.get()), 1);

  uint16_t bps = 0;
  uint16_t spp = 0;
  uint16_t compression = 0;
  uint16_t planar = 0;
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_BITSPERSAMPLE, &bps), 1);
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_SAMPLESPERPIXEL, &spp), 1);
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_COMPRESSION, &compression), 1);
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_PLANARCONFIG, &planar), 1);
  EXPECT_EQ(bps, 16U);
  EXPECT_EQ(spp, 3U);
  EXPECT_EQ(compression, COMPRESSION_LZW);
  EXPECT_EQ(planar, PLANARCONFIG_SEPARATE);

  ASSERT_EQ(TIFFScanlineSize(reader.get()),
            static_cast<tmsize_t>(kWidth * sizeof(uint16_t)));
  std::vector<uint16_t> actual(kWidth);
  for (uint16_t sample = 0; sample < kSamplesPerPixel; ++sample) {
    for (uint32_t row = 0; row < kHeight; ++row) {
      SCOPED_TRACE(sample);
      SCOPED_TRACE(row);
      ASSERT_EQ(TIFFReadScanline(reader.get(), actual.data(), row, sample), 1);
      for (uint32_t col = 0; col < kWidth; ++col) {
        const uint16_t expected =
            static_cast<uint16_t>(0x1000U * (sample + 1U) + row * 32U + col);
        EXPECT_EQ(actual[col], expected);
      }
    }
  }
}

}  // namespace
}  // namespace libtiff
