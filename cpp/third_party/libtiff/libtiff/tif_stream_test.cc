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

// Clean-room unit tests for libtiff C++ stream I/O (libtiff/tif_stream.cxx).
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_stream.cxx

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>
#include <istream>
#include <optional>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffio.hxx"

namespace libtiff {
namespace {

// Silent global error/warning callbacks for negative TIFFStreamOpen tests,
// installed via ScopedTiffErrorHandlers so stderr stays quiet.
struct GlobalLogCapture {
  static void Reset() {
    error_count = 0;
    warning_count = 0;
  }

  static void OnError(const char* /*module*/, const char* /*fmt*/,
                      va_list /*ap*/) {
    ++error_count;
  }

  static void OnWarning(const char* /*module*/, const char* /*fmt*/,
                        va_list /*ap*/) {
    ++warning_count;
  }

  static inline size_t error_count = 0;
  static inline size_t warning_count = 0;
};

// Custom output streambuf mimicking streams whose tellp() returns -1 until
// at least one byte has been written (e.g., empty ostrstream/ostringstream).
class UnpositionedInitialOutBuf : public std::streambuf {
 public:
  const std::string& str() const { return buf_; }
  bool saw_negative_tellp() const { return saw_negative_tellp_; }

 protected:
  int_type overflow(int_type ch) override {
    if (traits_type::eq_int_type(ch, traits_type::eof())) {
      return traits_type::not_eof(ch);
    }
    positioned_ = true;
    if (pos_ > buf_.size()) {
      buf_.resize(pos_, '\0');
    }
    if (pos_ == buf_.size()) {
      buf_.push_back(traits_type::to_char_type(ch));
    } else {
      buf_[pos_] = traits_type::to_char_type(ch);
    }
    ++pos_;
    return ch;
  }

  std::streamsize xsputn(const char* s, std::streamsize n) override {
    if (n <= 0) return 0;
    positioned_ = true;
    const size_t count = static_cast<size_t>(n);
    if (pos_ + count > buf_.size()) {
      buf_.resize(pos_ + count, '\0');
    }
    std::memcpy(buf_.data() + pos_, s, count);
    pos_ += count;
    return n;
  }

  pos_type seekoff(off_type off, std::ios_base::seekdir way,
                   std::ios_base::openmode which) override {
    if ((which & std::ios_base::out) == 0) {
      return pos_type(off_type(-1));
    }
    if (!positioned_ && off == 0 && way == std::ios_base::cur) {
      saw_negative_tellp_ = true;
      return pos_type(off_type(-1));
    }
    size_t base = 0;
    if (way == std::ios_base::cur) {
      base = pos_;
    } else if (way == std::ios_base::end) {
      base = buf_.size();
    }
    const off_type next = static_cast<off_type>(base) + off;
    if (next < 0 || static_cast<size_t>(next) > buf_.size()) {
      return pos_type(off_type(-1));
    }
    pos_ = static_cast<size_t>(next);
    positioned_ = true;
    return pos_type(next);
  }

  pos_type seekpos(pos_type sp, std::ios_base::openmode which) override {
    return seekoff(off_type(sp), std::ios_base::beg, which);
  }

 private:
  std::string buf_;
  size_t pos_ = 0;
  bool positioned_ = false;
  bool saw_negative_tellp_ = false;
};

// Configurable output streambuf for injecting write and seek failures.
class ConfigurableOutBuf : public std::streambuf {
 public:
  void set_max_bytes(std::optional<size_t> max_bytes) {
    max_bytes_ = max_bytes;
  }
  void set_fail_seek_beg_zero(bool fail) { fail_seek_beg_zero_ = fail; }
  void set_fail_all_seeks(bool fail) { fail_all_seeks_ = fail; }
  const std::string& str() const { return buf_; }

 protected:
  int_type overflow(int_type ch) override {
    if (traits_type::eq_int_type(ch, traits_type::eof())) {
      return traits_type::not_eof(ch);
    }
    if (max_bytes_.has_value() && pos_ >= *max_bytes_) {
      return traits_type::eof();
    }
    if (pos_ >= buf_.size()) {
      buf_.resize(pos_ + 1, '\0');
    }
    buf_[pos_++] = traits_type::to_char_type(ch);
    return ch;
  }

  std::streamsize xsputn(const char* s, std::streamsize n) override {
    if (n <= 0) return 0;
    size_t count = static_cast<size_t>(n);
    if (max_bytes_.has_value()) {
      if (pos_ >= *max_bytes_) return 0;
      count = std::min(count, *max_bytes_ - pos_);
    }
    if (pos_ + count > buf_.size()) {
      buf_.resize(pos_ + count, '\0');
    }
    std::memcpy(buf_.data() + pos_, s, count);
    pos_ += count;
    return static_cast<std::streamsize>(count);
  }

  pos_type seekoff(off_type off, std::ios_base::seekdir way,
                   std::ios_base::openmode which) override {
    if ((which & std::ios_base::out) == 0) {
      return pos_type(off_type(-1));
    }
    if (fail_all_seeks_) {
      return pos_type(off_type(-1));
    }
    if (fail_seek_beg_zero_ && way == std::ios_base::beg && off == 0) {
      return pos_type(off_type(-1));
    }
    size_t base = 0;
    if (way == std::ios_base::cur) {
      base = pos_;
    } else if (way == std::ios_base::end) {
      base = buf_.size();
    }
    const off_type next = static_cast<off_type>(base) + off;
    if (next < 0 || static_cast<size_t>(next) > buf_.size()) {
      return pos_type(off_type(-1));
    }
    pos_ = static_cast<size_t>(next);
    return pos_type(next);
  }

  pos_type seekpos(pos_type sp, std::ios_base::openmode which) override {
    return seekoff(off_type(sp), std::ios_base::beg, which);
  }

 private:
  std::string buf_;
  size_t pos_ = 0;
  std::optional<size_t> max_bytes_;
  bool fail_seek_beg_zero_ = false;
  bool fail_all_seeks_ = false;
};

// Configurable input streambuf for injecting short reads or seek failures.
class ConfigurableInBuf : public std::streambuf {
 public:
  explicit ConfigurableInBuf(std::string data) : buf_(std::move(data)) {}

  void set_max_readable_bytes(std::optional<size_t> limit) {
    max_readable_bytes_ = limit;
  }
  void set_fail_seeks(bool fail) { fail_seeks_ = fail; }

 protected:
  int_type underflow() override {
    const size_t avail_end = EffectiveSize();
    if (pos_ >= avail_end) {
      return traits_type::eof();
    }
    return traits_type::to_int_type(buf_[pos_]);
  }

  int_type uflow() override {
    const size_t avail_end = EffectiveSize();
    if (pos_ >= avail_end) {
      return traits_type::eof();
    }
    return traits_type::to_int_type(buf_[pos_++]);
  }

  std::streamsize xsgetn(char* s, std::streamsize n) override {
    if (n <= 0) return 0;
    const size_t avail_end = EffectiveSize();
    if (pos_ >= avail_end) return 0;
    const size_t to_read = std::min(static_cast<size_t>(n), avail_end - pos_);
    std::memcpy(s, buf_.data() + pos_, to_read);
    pos_ += to_read;
    return static_cast<std::streamsize>(to_read);
  }

  pos_type seekoff(off_type off, std::ios_base::seekdir way,
                   std::ios_base::openmode which) override {
    if ((which & std::ios_base::in) == 0 || fail_seeks_) {
      return pos_type(off_type(-1));
    }
    size_t base = 0;
    if (way == std::ios_base::cur) {
      base = pos_;
    } else if (way == std::ios_base::end) {
      base = buf_.size();
    }
    const off_type next = static_cast<off_type>(base) + off;
    if (next < 0 || static_cast<size_t>(next) > buf_.size()) {
      return pos_type(off_type(-1));
    }
    pos_ = static_cast<size_t>(next);
    return pos_type(next);
  }

  pos_type seekpos(pos_type sp, std::ios_base::openmode which) override {
    return seekoff(off_type(sp), std::ios_base::beg, which);
  }

 private:
  size_t EffectiveSize() const {
    if (!max_readable_bytes_.has_value()) return buf_.size();
    return std::min(buf_.size(), *max_readable_bytes_);
  }

  std::string buf_;
  size_t pos_ = 0;
  std::optional<size_t> max_readable_bytes_;
  bool fail_seeks_ = false;
};

uint8_t ExpectedRgbByte(uint32_t row, size_t col_byte, uint8_t seed) {
  return static_cast<uint8_t>((seed + row * 19U + col_byte * 7U) & 0xffU);
}

void WriteRgbStripedImage(TIFF* tif, uint32_t width, uint32_t height,
                          uint32_t rows_per_strip, uint16_t compression,
                          uint8_t seed, std::string_view description) {
  ASSERT_NE(tif, nullptr);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, compression), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, uint16_t{PHOTOMETRIC_RGB}),
            1);
  EXPECT_EQ(
      TIFFSetField(tif, TIFFTAG_PLANARCONFIG, uint16_t{PLANARCONFIG_CONTIG}),
      1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_XRESOLUTION, 150.0f), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_YRESOLUTION, 300.0f), 1);
  if (!description.empty()) {
    std::string desc_copy(description);
    EXPECT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEDESCRIPTION, desc_copy.c_str()),
              1);
  }

  const size_t row_bytes = static_cast<size_t>(width) * 3;
  for (uint32_t row = 0; row < height; ++row) {
    SCOPED_TRACE(row);
    std::vector<uint8_t> mutable_row(row_bytes);
    for (size_t i = 0; i < row_bytes; ++i) {
      mutable_row[i] = ExpectedRgbByte(row, i, seed);
    }
    ASSERT_EQ(TIFFWriteScanline(tif, mutable_row.data(), row, 0), 1);
  }
}

void VerifyRgbStripedImage(TIFF* tif, uint32_t expected_width,
                           uint32_t expected_height, uint8_t seed,
                           std::string_view expected_description) {
  ASSERT_NE(tif, nullptr);
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t bps = 0;
  uint16_t spp = 0;
  uint16_t planar = 0;
  float xres = 0.0f;
  float yres = 0.0f;
  EXPECT_EQ(TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width), 1);
  EXPECT_EQ(TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height), 1);
  EXPECT_EQ(TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bps), 1);
  EXPECT_EQ(TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp), 1);
  EXPECT_EQ(TIFFGetField(tif, TIFFTAG_PLANARCONFIG, &planar), 1);
  EXPECT_EQ(TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres), 1);
  EXPECT_EQ(TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yres), 1);
  EXPECT_EQ(width, expected_width);
  EXPECT_EQ(height, expected_height);
  EXPECT_EQ(bps, 8);
  EXPECT_EQ(spp, 3);
  EXPECT_EQ(planar, PLANARCONFIG_CONTIG);
  EXPECT_FLOAT_EQ(xres, 150.0f);
  EXPECT_FLOAT_EQ(yres, 300.0f);

  if (!expected_description.empty()) {
    const char* desc = nullptr;
    EXPECT_EQ(TIFFGetField(tif, TIFFTAG_IMAGEDESCRIPTION, &desc), 1);
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(std::string_view(desc), expected_description);
  }

  const size_t row_bytes = static_cast<size_t>(expected_width) * 3;
  std::vector<uint8_t> row_buf(row_bytes);
  for (uint32_t row = 0; row < expected_height; ++row) {
    SCOPED_TRACE(row);
    ASSERT_EQ(TIFFReadScanline(tif, row_buf.data(), row, 0), 1);
    for (size_t i = 0; i < row_bytes; ++i) {
      EXPECT_EQ(row_buf[i], ExpectedRgbByte(row, i, seed));
    }
  }
}

std::vector<uint8_t> BuildGrayMultiDirBytes(const char* mode, uint32_t width,
                                            uint32_t height, uint8_t seed0,
                                            uint8_t seed1) {
  MemoryTiffStream mem;
  {
    UniqueTiff writer = mem.Open(mode, nullptr, "mem_builder.tif");
    if (!writer) return {};

    auto write_dir = [&](uint8_t seed) {
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, width), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, height), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_NONE}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_MINISBLACK}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, height), 1);
      for (uint32_t row = 0; row < height; ++row) {
        std::vector<uint8_t> buf(width);
        for (uint32_t col = 0; col < width; ++col) {
          buf[col] = static_cast<uint8_t>((seed + row * 13U + col * 5U) & 0xff);
        }
        EXPECT_EQ(TIFFWriteScanline(writer.get(), buf.data(), row, 0), 1);
      }
      EXPECT_EQ(TIFFWriteDirectory(writer.get()), 1);
    };

    write_dir(seed0);
    write_dir(seed1);
  }
  return mem.data();
}

TEST(TifStreamTest, OStringStreamWriteAndIStringStreamReadRoundTrip) {
  GlobalLogCapture::Reset();
  ScopedTiffErrorHandlers handlers(&GlobalLogCapture::OnError,
                                   &GlobalLogCapture::OnWarning);

  std::ostringstream os;
  {
    UniqueTiff writer(TIFFStreamOpen("roundtrip.tif", &os));
    ASSERT_NE(writer, nullptr);
    EXPECT_STREQ(TIFFFileName(writer.get()), "roundtrip.tif");
    EXPECT_EQ(TIFFIsBigEndian(writer.get()), 0);
    EXPECT_EQ(TIFFIsBigTIFF(writer.get()), 0);
    EXPECT_EQ(TIFFIsTiled(writer.get()), 0);
    WriteRgbStripedImage(writer.get(), /*width=*/16, /*height=*/12,
                         /*rows_per_strip=*/4, COMPRESSION_NONE, /*seed=*/23,
                         "stream-roundtrip");
  }

  const std::string encoded = os.str();
  ASSERT_GT(encoded.size(), sizeof(TIFFHeaderClassic));

  std::istringstream is(encoded);
  UniqueTiff reader(TIFFStreamOpen("roundtrip.tif", &is));
  ASSERT_NE(reader, nullptr);
  VerifyRgbStripedImage(reader.get(), /*expected_width=*/16,
                        /*expected_height=*/12, /*seed=*/23,
                        "stream-roundtrip");
  EXPECT_EQ(TIFFReadDirectory(reader.get()), 0);
  EXPECT_EQ(GlobalLogCapture::error_count, 0u);
}

TEST(TifStreamTest, StringStreamBidirectionalAndMultiDirectoryIStream) {
  GlobalLogCapture::Reset();
  ScopedTiffErrorHandlers handlers(&GlobalLogCapture::OnError,
                                   &GlobalLogCapture::OnWarning);

  // Single-directory write/read round-trip on the same std::stringstream
  // via ostream* and istream* overloads of TIFFStreamOpen.
  std::stringstream ss;
  {
    UniqueTiff writer(
        TIFFStreamOpen("bidir.tif", static_cast<std::ostream*>(&ss)));
    ASSERT_NE(writer, nullptr);
    WriteRgbStripedImage(writer.get(), /*width=*/10, /*height=*/6,
                         /*rows_per_strip=*/3, COMPRESSION_LZW, /*seed=*/11,
                         "page-0");
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  ss.seekg(0, std::ios::beg);
  {
    UniqueTiff reader(
        TIFFStreamOpen("bidir.tif", static_cast<std::istream*>(&ss)));
    ASSERT_NE(reader, nullptr);
    VerifyRgbStripedImage(reader.get(), 10, 6, 11, "page-0");
    EXPECT_EQ(TIFFReadDirectory(reader.get()), 0);
  }

  // Multi-directory reading over std::stringstream as std::istream* exercises
  // SEEK_CUR (TIFFNumberOfDirectories) and backward SEEK_SET
  // (TIFFSetDirectory).
  const std::vector<uint8_t> multi_bytes =
      BuildGrayMultiDirBytes("w4l", /*width=*/8, /*height=*/5,
                             /*seed0=*/33, /*seed1=*/99);
  ASSERT_FALSE(multi_bytes.empty());
  std::stringstream multi_ss(std::string(
      reinterpret_cast<const char*>(multi_bytes.data()), multi_bytes.size()));
  UniqueTiff multi_reader(
      TIFFStreamOpen("multi.tif", static_cast<std::istream*>(&multi_ss)));
  ASSERT_NE(multi_reader, nullptr);
  EXPECT_EQ(TIFFNumberOfDirectories(multi_reader.get()), 2u);
  for (const auto& [dir, seed] :
       std::array<std::pair<tdir_t, uint8_t>, 2>{{{0, 33}, {1, 99}}}) {
    ASSERT_EQ(TIFFSetDirectory(multi_reader.get(), dir), 1);
    std::array<uint8_t, 8> row_buf{};
    for (uint32_t row = 0; row < 5; ++row) {
      ASSERT_EQ(TIFFReadScanline(multi_reader.get(), row_buf.data(), row, 0),
                1);
      for (uint32_t col = 0; col < 8; ++col) {
        EXPECT_EQ(row_buf[col],
                  static_cast<uint8_t>((seed + row * 13U + col * 5U) & 0xff));
      }
    }
  }
  EXPECT_EQ(GlobalLogCapture::error_count, 0u);
}

TEST(TifStreamTest, ClassicAndBigTiffLittleAndBigEndianIStream) {
  GlobalLogCapture::Reset();
  ScopedTiffErrorHandlers handlers(&GlobalLogCapture::OnError,
                                   &GlobalLogCapture::OnWarning);

  struct FormatCase {
    const char* name;
    const char* open_mode;
    int expected_big_endian;
    int expected_big_tiff;
  };
  constexpr std::array<FormatCase, 4> kCases = {{
      {.name = "classic-le",
       .open_mode = "w4l",
       .expected_big_endian = 0,
       .expected_big_tiff = 0},
      {.name = "classic-be",
       .open_mode = "w4b",
       .expected_big_endian = 1,
       .expected_big_tiff = 0},
      {.name = "bigtiff-le",
       .open_mode = "w8l",
       .expected_big_endian = 0,
       .expected_big_tiff = 1},
      {.name = "bigtiff-be",
       .open_mode = "w8b",
       .expected_big_endian = 1,
       .expected_big_tiff = 1},
  }};

  for (const FormatCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const std::vector<uint8_t> bytes =
        BuildGrayMultiDirBytes(tc.open_mode, /*width=*/7, /*height=*/5,
                               /*seed0=*/31, /*seed1=*/77);
    ASSERT_FALSE(bytes.empty());

    std::string raw(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::istringstream is(raw);
    UniqueTiff reader(TIFFStreamOpen(tc.name, &is));
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(TIFFIsBigEndian(reader.get()), tc.expected_big_endian);
    EXPECT_EQ(TIFFIsBigTIFF(reader.get()), tc.expected_big_tiff);
    EXPECT_EQ(TIFFNumberOfDirectories(reader.get()), 2u);

    auto verify_dir = [&](tdir_t dir_idx, uint8_t seed) {
      ASSERT_EQ(TIFFSetDirectory(reader.get(), dir_idx), 1);
      uint32_t width = 0;
      uint32_t height = 0;
      uint16_t bps = 0;
      uint16_t spp = 0;
      EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGEWIDTH, &width), 1);
      EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGELENGTH, &height), 1);
      EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_BITSPERSAMPLE, &bps), 1);
      EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_SAMPLESPERPIXEL, &spp), 1);
      EXPECT_EQ(width, 7u);
      EXPECT_EQ(height, 5u);
      EXPECT_EQ(bps, 8);
      EXPECT_EQ(spp, 1);

      std::vector<uint8_t> row_buf(width);
      for (uint32_t row = 0; row < height; ++row) {
        ASSERT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), row, 0), 1);
        for (uint32_t col = 0; col < width; ++col) {
          EXPECT_EQ(row_buf[col],
                    static_cast<uint8_t>((seed + row * 13U + col * 5U) & 0xff));
        }
      }
    };

    verify_dir(0, 31);
    verify_dir(1, 77);
  }
  EXPECT_EQ(GlobalLogCapture::error_count, 0u);
}

TEST(TifStreamTest, NonZeroInitialIStreamStartPosition) {
  GlobalLogCapture::Reset();
  ScopedTiffErrorHandlers handlers(&GlobalLogCapture::OnError,
                                   &GlobalLogCapture::OnWarning);

  const std::vector<uint8_t> bytes =
      BuildGrayMultiDirBytes("w8b", /*width=*/5, /*height=*/4,
                             /*seed0=*/42, /*seed1=*/84);
  ASSERT_FALSE(bytes.empty());

  constexpr std::string_view kPrefix = "NONZERO_PREFIX_17";
  std::string combined(kPrefix);
  combined.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());

  std::istringstream is(combined);
  is.seekg(static_cast<std::streamoff>(kPrefix.size()), std::ios::beg);
  ASSERT_FALSE(is.fail());

  UniqueTiff reader(TIFFStreamOpen("embedded.tif", &is));
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(TIFFIsBigEndian(reader.get()), 1);
  EXPECT_EQ(TIFFIsBigTIFF(reader.get()), 1);
  EXPECT_EQ(TIFFNumberOfDirectories(reader.get()), 2u);

  // _tiffisSeekProc subtracts start_pos when returning the TIFF offset.
  TIFFSeekProc seek_proc = TIFFGetSeekProc(reader.get());
  ASSERT_NE(seek_proc, nullptr);
  EXPECT_EQ(seek_proc(TIFFClientdata(reader.get()), 8, SEEK_SET), 8u);

  ASSERT_EQ(TIFFSetDirectory(reader.get(), 1), 1);
  std::vector<uint8_t> row_buf(5);
  for (uint32_t row = 0; row < 4; ++row) {
    ASSERT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), row, 0), 1);
    for (uint32_t col = 0; col < 5; ++col) {
      EXPECT_EQ(row_buf[col],
                static_cast<uint8_t>((84 + row * 13U + col * 5U) & 0xff));
    }
  }
  EXPECT_EQ(GlobalLogCapture::error_count, 0u);
}

TEST(TifStreamTest, BitDepthsSeparatePlanarAndTiledStreams) {
  GlobalLogCapture::Reset();
  ScopedTiffErrorHandlers handlers(&GlobalLogCapture::OnError,
                                   &GlobalLogCapture::OnWarning);

  // Note: TIFFStreamOpen(ostream*) sets _tiffosReadProc to return 0, so each
  // write-only stream holds a single directory (linking a second directory
  // requires reading back prior IFD headers).

  // 1-bit bilevel (16x4 -> 2 bytes per row).
  {
    std::ostringstream os;
    {
      UniqueTiff writer(TIFFStreamOpen("bilevel.tif", &os));
      ASSERT_NE(writer, nullptr);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
                1);
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_NONE}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_MINISBLACK}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{4}),
                1);
      for (uint32_t row = 0; row < 4; ++row) {
        std::array<uint8_t, 2> line = {
            static_cast<uint8_t>(0xA0U + row),
            static_cast<uint8_t>(0x50U + row * 3U),
        };
        ASSERT_EQ(TIFFWriteScanline(writer.get(), line.data(), row, 0), 1);
      }
    }
    std::istringstream is(os.str());
    UniqueTiff reader(TIFFStreamOpen("bilevel.tif", &is));
    ASSERT_NE(reader, nullptr);
    for (uint32_t row = 0; row < 4; ++row) {
      std::array<uint8_t, 2> line{};
      ASSERT_EQ(TIFFReadScanline(reader.get(), line.data(), row, 0), 1);
      EXPECT_EQ(line[0], static_cast<uint8_t>(0xA0U + row));
      EXPECT_EQ(line[1], static_cast<uint8_t>(0x50U + row * 3U));
    }
  }

  // 8-bit RGB PLANARCONFIG_SEPARATE.
  {
    std::ostringstream os;
    {
      UniqueTiff writer(TIFFStreamOpen("separate.tif", &os));
      ASSERT_NE(writer, nullptr);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{6}), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_NONE}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_RGB}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_SEPARATE}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{2}),
                1);
      for (uint16_t plane = 0; plane < 3; ++plane) {
        for (uint32_t row = 0; row < 4; ++row) {
          std::array<uint8_t, 6> line{};
          for (size_t x = 0; x < line.size(); ++x) {
            line[x] = static_cast<uint8_t>(plane * 50U + row * 7U + x);
          }
          ASSERT_EQ(TIFFWriteScanline(writer.get(), line.data(), row, plane),
                    1);
        }
      }
    }
    std::istringstream is(os.str());
    UniqueTiff reader(TIFFStreamOpen("separate.tif", &is));
    ASSERT_NE(reader, nullptr);
    for (uint16_t plane = 0; plane < 3; ++plane) {
      for (uint32_t row = 0; row < 4; ++row) {
        std::array<uint8_t, 6> line{};
        ASSERT_EQ(TIFFReadScanline(reader.get(), line.data(), row, plane), 1);
        for (size_t x = 0; x < line.size(); ++x) {
          EXPECT_EQ(line[x], static_cast<uint8_t>(plane * 50U + row * 7U + x));
        }
      }
    }
  }

  // 16-bit tiled grayscale (16x16 tile on 16x16 image).
  {
    std::ostringstream os;
    {
      UniqueTiff writer(TIFFStreamOpen("tiled16.tif", &os));
      ASSERT_NE(writer, nullptr);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{16}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, uint32_t{16}), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, uint32_t{16}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{16}),
                1);
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_NONE}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_MINISBLACK}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      std::vector<uint16_t> tile(16 * 16);
      for (size_t i = 0; i < tile.size(); ++i) {
        tile[i] = static_cast<uint16_t>(1000U + i * 17U);
      }
      ASSERT_NE(TIFFWriteEncodedTile(writer.get(), 0, tile.data(),
                                     static_cast<tmsize_t>(tile.size() * 2)),
                static_cast<tmsize_t>(-1));
    }
    std::istringstream is(os.str());
    UniqueTiff reader(TIFFStreamOpen("tiled16.tif", &is));
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFIsTiled(reader.get()), 1);
    std::vector<uint16_t> decoded_tile(16 * 16);
    const tmsize_t bytes_read =
        TIFFReadEncodedTile(reader.get(), 0, decoded_tile.data(),
                            static_cast<tmsize_t>(decoded_tile.size() * 2));
    EXPECT_EQ(bytes_read, static_cast<tmsize_t>(decoded_tile.size() * 2));
    for (size_t i = 0; i < decoded_tile.size(); ++i) {
      EXPECT_EQ(decoded_tile[i], static_cast<uint16_t>(1000U + i * 17U));
    }
  }
  EXPECT_EQ(GlobalLogCapture::error_count, 0u);
}

TEST(TifStreamTest, OStringStreamSeekPastEndPadsZeroBytesForIfdAlignment) {
  GlobalLogCapture::Reset();
  ScopedTiffErrorHandlers handlers(&GlobalLogCapture::OnError,
                                   &GlobalLogCapture::OnWarning);

  // A 3x3 8-bit image writes a 9-byte strip after the 8-byte header (17 bytes,
  // odd). TIFFWriteDirectory seeks to offset 18 (SEEK_SET) past EOF of the
  // ostringstream, triggering _tiffosSeekProc's failbit recovery + NUL padding.
  std::ostringstream os;
  {
    UniqueTiff writer(TIFFStreamOpen("odd_strip.tif", &os));
    ASSERT_NE(writer, nullptr);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{3}), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{3}), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                           uint16_t{COMPRESSION_NONE}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                           uint16_t{PHOTOMETRIC_MINISBLACK}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                           uint16_t{PLANARCONFIG_CONTIG}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{3}), 1);
    for (uint32_t row = 0; row < 3; ++row) {
      std::array<uint8_t, 3> line = {
          static_cast<uint8_t>(row * 10U + 1U),
          static_cast<uint8_t>(row * 10U + 2U),
          static_cast<uint8_t>(row * 10U + 3U),
      };
      ASSERT_EQ(TIFFWriteScanline(writer.get(), line.data(), row, 0), 1);
    }
  }

  const std::string encoded = os.str();
  // Byte 17 is the NUL padding byte inserted before the IFD at offset 18.
  ASSERT_GT(encoded.size(), 18u);
  EXPECT_EQ(encoded[17], '\0');

  std::istringstream is(encoded);
  UniqueTiff reader(TIFFStreamOpen("odd_strip.tif", &is));
  ASSERT_NE(reader, nullptr);
  for (uint32_t row = 0; row < 3; ++row) {
    std::array<uint8_t, 3> line{};
    ASSERT_EQ(TIFFReadScanline(reader.get(), line.data(), row, 0), 1);
    EXPECT_EQ(line[0], static_cast<uint8_t>(row * 10U + 1U));
    EXPECT_EQ(line[1], static_cast<uint8_t>(row * 10U + 2U));
    EXPECT_EQ(line[2], static_cast<uint8_t>(row * 10U + 3U));
  }
  EXPECT_EQ(GlobalLogCapture::error_count, 0u);
}

TEST(TifStreamTest, DirectCallbackContractsSeekSizeMapAndCrossMode) {
  GlobalLogCapture::Reset();
  ScopedTiffErrorHandlers handlers(&GlobalLogCapture::OnError,
                                   &GlobalLogCapture::OnWarning);

  // Test ostream callbacks via TIFFGet*Proc.
  std::ostringstream os;
  {
    UniqueTiff writer(TIFFStreamOpen("os_procs.tif", &os));
    ASSERT_NE(writer, nullptr);
    thandle_t cd = TIFFClientdata(writer.get());

    // _tiffosReadProc always returns 0.
    std::array<uint8_t, 4> dummy_buf = {1, 2, 3, 4};
    EXPECT_EQ(TIFFGetReadProc(writer.get())(cd, dummy_buf.data(), 4), 0);

    // _tiffDummyMapProcCxx / _tiffDummyUnmapProcCxx are no-ops returning 0.
    void* map_base = nullptr;
    toff_t map_size = 0;
    EXPECT_EQ(TIFFGetMapFileProc(writer.get())(cd, &map_base, &map_size), 0);
    TIFFGetUnmapFileProc(writer.get())(cd, nullptr, 0);

    // _tiffosSizeProc reports the 8-byte header written on open.
    TIFFSizeProc size_proc = TIFFGetSizeProc(writer.get());
    EXPECT_EQ(size_proc(cd), 8u);

    TIFFSeekProc seek_proc = TIFFGetSeekProc(writer.get());
    // SEEK_SET, SEEK_CUR within bounds, and SEEK_END within bounds.
    EXPECT_EQ(seek_proc(cd, 4, SEEK_SET), 4u);
    EXPECT_EQ(seek_proc(cd, 2, SEEK_CUR), 6u);
    EXPECT_EQ(seek_proc(cd, static_cast<uint64_t>(static_cast<int64_t>(-2)),
                        SEEK_CUR),
              4u);
    EXPECT_EQ(seek_proc(cd, 0, SEEK_END), 8u);
    EXPECT_EQ(seek_proc(cd, static_cast<uint64_t>(static_cast<int64_t>(-3)),
                        SEEK_END),
              5u);

    // SEEK_CUR past EOF on ostringstream triggers failbit recovery + NUL fill.
    EXPECT_EQ(seek_proc(cd, 6, SEEK_CUR), 11u);
    EXPECT_EQ(size_proc(cd), 11u);

    // SEEK_END past EOF on ostringstream triggers failbit recovery + NUL fill.
    EXPECT_EQ(seek_proc(cd, 3, SEEK_END), 14u);
    EXPECT_EQ(size_proc(cd), 14u);

    // If the ostream has already failed, _tiffosSeekProc returns -1
    // immediately.
    os.setstate(std::ios::failbit);
    EXPECT_EQ(seek_proc(cd, 0, SEEK_SET), static_cast<uint64_t>(-1));
    os.clear();
  }

  // Test istream callbacks via TIFFGet*Proc.
  const std::vector<uint8_t> bytes =
      BuildGrayMultiDirBytes("w4l", /*width=*/4, /*height=*/4, 1, 2);
  std::string raw(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  std::istringstream is(raw);
  {
    UniqueTiff reader(TIFFStreamOpen("is_procs.tif", &is));
    ASSERT_NE(reader, nullptr);
    thandle_t cd = TIFFClientdata(reader.get());

    // _tiffisWriteProc always returns 0.
    std::array<uint8_t, 4> dummy_buf = {1, 2, 3, 4};
    EXPECT_EQ(TIFFGetWriteProc(reader.get())(cd, dummy_buf.data(), 4), 0);

    // _tiffDummyMapProcCxx / _tiffDummyUnmapProcCxx.
    void* map_base = nullptr;
    toff_t map_size = 0;
    EXPECT_EQ(TIFFGetMapFileProc(reader.get())(cd, &map_base, &map_size), 0);
    TIFFGetUnmapFileProc(reader.get())(cd, nullptr, 0);

    // _tiffisSizeProc and _tiffisSeekProc across SEEK_SET / SEEK_CUR /
    // SEEK_END.
    EXPECT_EQ(TIFFGetSizeProc(reader.get())(cd), raw.size());
    TIFFSeekProc seek_proc = TIFFGetSeekProc(reader.get());
    EXPECT_EQ(seek_proc(cd, 4, SEEK_SET), 4u);
    EXPECT_EQ(seek_proc(cd, 4, SEEK_CUR), 8u);
    EXPECT_EQ(seek_proc(cd, static_cast<uint64_t>(static_cast<int64_t>(-2)),
                        SEEK_CUR),
              6u);
    EXPECT_EQ(seek_proc(cd, 0, SEEK_END), raw.size());
    EXPECT_EQ(seek_proc(cd, static_cast<uint64_t>(static_cast<int64_t>(-4)),
                        SEEK_END),
              raw.size() - 4);
  }
  EXPECT_EQ(GlobalLogCapture::error_count, 0u);
}

TEST(TifStreamTest, UnpositionedInitialOutStreamWorkaround) {
  GlobalLogCapture::Reset();
  ScopedTiffErrorHandlers handlers(&GlobalLogCapture::OnError,
                                   &GlobalLogCapture::OnWarning);

  UnpositionedInitialOutBuf out_buf;
  std::ostream os(&out_buf);
  {
    UniqueTiff writer(TIFFStreamOpen("unpositioned.tif", &os));
    ASSERT_NE(writer, nullptr);
    EXPECT_TRUE(out_buf.saw_negative_tellp());
    WriteRgbStripedImage(writer.get(), /*width=*/4, /*height=*/3,
                         /*rows_per_strip=*/3, COMPRESSION_NONE, /*seed=*/5,
                         "workaround");
  }

  std::istringstream is(out_buf.str());
  UniqueTiff reader(TIFFStreamOpen("unpositioned.tif", &is));
  ASSERT_NE(reader, nullptr);
  VerifyRgbStripedImage(reader.get(), 4, 3, 5, "workaround");
  EXPECT_EQ(GlobalLogCapture::error_count, 0u);
}

TEST(TifStreamTest, PreFailedAndNonSeekableStreamsFailOpenCleanly) {
  GlobalLogCapture::Reset();
  ScopedTiffErrorHandlers handlers(&GlobalLogCapture::OnError,
                                   &GlobalLogCapture::OnWarning);

  // Pre-failed ostream (failbit and badbit) exercises !os->fail() false and
  // delete data on failed open.
  {
    std::ostringstream os_fail;
    os_fail.setstate(std::ios::failbit);
    UniqueTiff tif(TIFFStreamOpen("fail_os.tif", &os_fail));
    EXPECT_EQ(tif, nullptr);
  }
  {
    std::ostringstream os_bad;
    os_bad.setstate(std::ios::badbit);
    UniqueTiff tif(TIFFStreamOpen("bad_os.tif", &os_bad));
    EXPECT_EQ(tif, nullptr);
  }

  // Pre-failed istream.
  {
    std::istringstream is_fail(
        std::string("\x49\x49\x2a\x00\x08\x00\x00\x00", 8), std::ios::binary);
    is_fail.setstate(std::ios::failbit);
    UniqueTiff tif(TIFFStreamOpen("fail_is.tif", &is_fail));
    EXPECT_EQ(tif, nullptr);
  }

  // Unseekable custom out streambuf fails during header seek/write.
  {
    ConfigurableOutBuf out_buf;
    out_buf.set_fail_all_seeks(true);
    std::ostream os(&out_buf);
    UniqueTiff tif(TIFFStreamOpen("nonseek_os.tif", &os));
    EXPECT_EQ(tif, nullptr);
    EXPECT_TRUE(os.fail());
  }

  // Empty, truncated, and corrupt istreams hit !tif -> delete data cleanly.
  const std::array<std::string_view, 5> kBadInputs = {
      "",
      "II*",
      std::string_view("XX\x2a\x00\x08\x00\x00\x00", 8),
      std::string_view("II\x2b\x00\x08\x00", 6),
      std::string_view("II\x2a\x00\x08\x00\x00\x00\x00\x00", 10),
  };
  for (std::string_view bad_bytes : kBadInputs) {
    std::istringstream is(std::string(bad_bytes), std::ios::binary);
    UniqueTiff tif(TIFFStreamOpen("bad_is.tif", &is));
    EXPECT_EQ(tif, nullptr);
  }
  EXPECT_GT(GlobalLogCapture::error_count, 0u);
}

TEST(TifStreamTest, WriteAndSeekFailureInjectionOnCustomOutBuf) {
  GlobalLogCapture::Reset();
  ScopedTiffErrorHandlers handlers(&GlobalLogCapture::OnError,
                                   &GlobalLogCapture::OnWarning);

  // Header write fails after 0 bytes and after 4 bytes.
  for (size_t limit : {size_t{0}, size_t{4}}) {
    ConfigurableOutBuf out_buf;
    out_buf.set_max_bytes(limit);
    std::ostream os(&out_buf);
    UniqueTiff tif(TIFFStreamOpen("header_write_fail.tif", &os));
    EXPECT_EQ(tif, nullptr);
    EXPECT_TRUE(os.fail());
  }

  // Header succeeds (8 bytes), pixel flush/directory write fails at 16 bytes.
  {
    ConfigurableOutBuf out_buf;
    out_buf.set_max_bytes(16);
    std::ostream os(&out_buf);
    UniqueTiff tif(TIFFStreamOpen("mid_write_fail.tif", &os));
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{8}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION,
                           uint16_t{COMPRESSION_NONE}),
              1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC,
                           uint16_t{PHOTOMETRIC_MINISBLACK}),
              1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG,
                           uint16_t{PLANARCONFIG_CONTIG}),
              1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{4}), 1);
    for (uint32_t row = 0; row < 4; ++row) {
      std::array<uint8_t, 8> line{};
      EXPECT_EQ(TIFFWriteScanline(tif.get(), line.data(), row, 0), 1);
    }
    EXPECT_EQ(TIFFWriteDirectory(tif.get()), 0);
    EXPECT_TRUE(os.bad());
  }

  // Seek to SEEK_SET offset 0 fails when origin + off == start_pos, exercising
  // the false branch of (origin + off) > start_pos in _tiffosSeekProc.
  {
    ConfigurableOutBuf out_buf;
    std::ostream os(&out_buf);
    UniqueTiff tif(TIFFStreamOpen("seek_zero_fail.tif", &os));
    ASSERT_NE(tif, nullptr);
    out_buf.set_fail_seek_beg_zero(true);
    TIFFSeekProc seek_proc = TIFFGetSeekProc(tif.get());
    EXPECT_EQ(seek_proc(TIFFClientdata(tif.get()), 0, SEEK_SET),
              static_cast<uint64_t>(-1));
    EXPECT_TRUE(os.fail());
    // Subsequent seek hits early if (os->fail()) return -1.
    EXPECT_EQ(seek_proc(TIFFClientdata(tif.get()), 0, SEEK_SET),
              static_cast<uint64_t>(-1));
    // Clear stream failure and allow seek before UniqueTiff closes cleanly.
    out_buf.set_fail_seek_beg_zero(false);
    os.clear();
  }
  EXPECT_GT(GlobalLogCapture::error_count, 0u);
}

TEST(TifStreamTest, ReadAndSeekFailureInjectionOnCustomInBuf) {
  GlobalLogCapture::Reset();
  ScopedTiffErrorHandlers handlers(&GlobalLogCapture::OnError,
                                   &GlobalLogCapture::OnWarning);

  const std::vector<uint8_t> bytes =
      BuildGrayMultiDirBytes("w4l", /*width=*/16, /*height=*/8, 10, 20);
  ASSERT_GT(bytes.size(), 64u);
  std::string raw(reinterpret_cast<const char*>(bytes.data()), bytes.size());

  // Valid header + directory, but strip payload truncated mid-read.
  {
    ConfigurableInBuf in_buf(raw);
    std::istream is(&in_buf);
    UniqueTiff tif(TIFFStreamOpen("truncated_read.tif", &is));
    ASSERT_NE(tif, nullptr);
    in_buf.set_max_readable_bytes(16);
    std::vector<uint8_t> row_buf(16);
    EXPECT_EQ(TIFFReadScanline(tif.get(), row_buf.data(), 0, 0), -1);
  }

  // Injected badbit on istream causes read failures.
  {
    ConfigurableInBuf in_buf(raw);
    std::istream is(&in_buf);
    UniqueTiff tif(TIFFStreamOpen("badbit_read.tif", &is));
    ASSERT_NE(tif, nullptr);
    is.setstate(std::ios::badbit);
    std::vector<uint8_t> row_buf(16);
    EXPECT_EQ(TIFFReadScanline(tif.get(), row_buf.data(), 0, 0), -1);
    EXPECT_TRUE(is.bad());
  }

  // Seek failure during directory read causes TIFFSetDirectory to fail.
  {
    ConfigurableInBuf in_buf(raw);
    std::istream is(&in_buf);
    UniqueTiff tif(TIFFStreamOpen("seek_fail_read.tif", &is));
    ASSERT_NE(tif, nullptr);
    in_buf.set_fail_seeks(true);
    EXPECT_EQ(TIFFSetDirectory(tif.get(), 1), 0);
  }
  EXPECT_GT(GlobalLogCapture::error_count, 0u);
}

}  // namespace
}  // namespace libtiff
