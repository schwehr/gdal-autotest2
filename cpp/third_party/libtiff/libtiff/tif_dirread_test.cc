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

#include <fcntl.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tif_dir.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

using ::testing::HasSubstr;

struct FormatParam {
  bool big_endian;
  bool big_tiff;
  bool mmap;
  const char* name;
};

constexpr std::array<FormatParam, 8> kFormatParams = {{
    {false, false, false, "ClassicLEUnmapped"},
    {true, false, false, "ClassicBEUnmapped"},
    {false, true, false, "BigTiffLEUnmapped"},
    {true, true, false, "BigTiffBEUnmapped"},
    {false, false, true, "ClassicLEMmap"},
    {true, false, true, "ClassicBEMmap"},
    {false, true, true, "BigTiffLEMmap"},
    {true, true, true, "BigTiffBEMmap"},
}};

const char* ReadMode(bool mmap) { return mmap ? "rM" : "rm"; }
const char* HeaderOnlyMode(bool mmap) { return mmap ? "rhM" : "rhm"; }

void EncodeU16(uint16_t v, bool be, uint8_t* dst) {
  if (be) {
    dst[0] = static_cast<uint8_t>((v >> 8) & 0xffU);
    dst[1] = static_cast<uint8_t>(v & 0xffU);
  } else {
    dst[0] = static_cast<uint8_t>(v & 0xffU);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xffU);
  }
}

void EncodeU32(uint32_t v, bool be, uint8_t* dst) {
  for (int i = 0; i < 4; ++i) {
    const int shift = be ? (24 - 8 * i) : (8 * i);
    dst[i] = static_cast<uint8_t>((v >> shift) & 0xffU);
  }
}

void EncodeU64(uint64_t v, bool be, uint8_t* dst) {
  for (int i = 0; i < 8; ++i) {
    const int shift = be ? (56 - 8 * i) : (8 * i);
    dst[i] = static_cast<uint8_t>((v >> shift) & 0xffULL);
  }
}

RawIfdEntry MakeBytesEntry(const RawTiffBuilder& b, uint16_t tag, uint16_t type,
                           uint64_t count, std::vector<uint8_t> raw) {
  if (raw.size() <= b.InlineSlotSize()) {
    RawIfdEntry e{.tag = tag, .type = type, .count = count};
    std::copy(raw.begin(), raw.end(), e.inline_bytes.begin());
    return e;
  }
  return b.MakeOutOfLine(tag, type, count, std::move(raw));
}

RawIfdEntry MakeSbyteEntry(const RawTiffBuilder& b, uint16_t tag,
                           int8_t value) {
  return b.MakeByte(tag, static_cast<uint8_t>(value), TIFF_SBYTE);
}

RawIfdEntry MakeSbyteArrayEntry(const RawTiffBuilder& b, uint16_t tag,
                                absl::Span<const int8_t> vals) {
  std::vector<uint8_t> raw(vals.size());
  for (size_t i = 0; i < vals.size(); ++i) {
    raw[i] = static_cast<uint8_t>(vals[i]);
  }
  return MakeBytesEntry(b, tag, TIFF_SBYTE, vals.size(), std::move(raw));
}

RawIfdEntry MakeSshortEntry(const RawTiffBuilder& b, uint16_t tag,
                            int16_t value) {
  return b.MakeShort(tag, std::bit_cast<uint16_t>(value), TIFF_SSHORT);
}

RawIfdEntry MakeSshortArrayEntry(const RawTiffBuilder& b, uint16_t tag,
                                 absl::Span<const int16_t> vals) {
  std::vector<uint8_t> raw(vals.size() * 2);
  for (size_t i = 0; i < vals.size(); ++i) {
    EncodeU16(std::bit_cast<uint16_t>(vals[i]), b.big_endian(),
              raw.data() + i * 2);
  }
  return MakeBytesEntry(b, tag, TIFF_SSHORT, vals.size(), std::move(raw));
}

RawIfdEntry MakeSlongArrayEntry(const RawTiffBuilder& b, uint16_t tag,
                                absl::Span<const int32_t> vals) {
  std::vector<uint8_t> raw(vals.size() * 4);
  for (size_t i = 0; i < vals.size(); ++i) {
    EncodeU32(std::bit_cast<uint32_t>(vals[i]), b.big_endian(),
              raw.data() + i * 4);
  }
  return MakeBytesEntry(b, tag, TIFF_SLONG, vals.size(), std::move(raw));
}

RawIfdEntry MakeSlong8Entry(const RawTiffBuilder& b, uint16_t tag,
                            int64_t value) {
  return b.MakeLong8(tag, std::bit_cast<uint64_t>(value), TIFF_SLONG8);
}

RawIfdEntry MakeSlong8ArrayEntry(const RawTiffBuilder& b, uint16_t tag,
                                 absl::Span<const int64_t> vals) {
  std::vector<uint8_t> raw(vals.size() * 8);
  for (size_t i = 0; i < vals.size(); ++i) {
    EncodeU64(std::bit_cast<uint64_t>(vals[i]), b.big_endian(),
              raw.data() + i * 8);
  }
  return MakeBytesEntry(b, tag, TIFF_SLONG8, vals.size(), std::move(raw));
}

RawIfdEntry MakeFloatArrayEntry(const RawTiffBuilder& b, uint16_t tag,
                                absl::Span<const float> vals) {
  std::vector<uint8_t> raw(vals.size() * 4);
  for (size_t i = 0; i < vals.size(); ++i) {
    EncodeU32(std::bit_cast<uint32_t>(vals[i]), b.big_endian(),
              raw.data() + i * 4);
  }
  return MakeBytesEntry(b, tag, TIFF_FLOAT, vals.size(), std::move(raw));
}

RawIfdEntry MakeDoubleArrayEntry(const RawTiffBuilder& b, uint16_t tag,
                                 absl::Span<const double> vals) {
  std::vector<uint8_t> raw(vals.size() * 8);
  for (size_t i = 0; i < vals.size(); ++i) {
    EncodeU64(std::bit_cast<uint64_t>(vals[i]), b.big_endian(),
              raw.data() + i * 8);
  }
  return MakeBytesEntry(b, tag, TIFF_DOUBLE, vals.size(), std::move(raw));
}

RawIfdEntry MakeRationalArrayEntry(
    const RawTiffBuilder& b, uint16_t tag,
    absl::Span<const std::pair<uint32_t, uint32_t>> vals) {
  std::vector<uint8_t> raw(vals.size() * 8);
  for (size_t i = 0; i < vals.size(); ++i) {
    EncodeU32(vals[i].first, b.big_endian(), raw.data() + i * 8);
    EncodeU32(vals[i].second, b.big_endian(), raw.data() + i * 8 + 4);
  }
  return MakeBytesEntry(b, tag, TIFF_RATIONAL, vals.size(), std::move(raw));
}

RawIfdEntry MakeSrationalArrayEntry(
    const RawTiffBuilder& b, uint16_t tag,
    absl::Span<const std::pair<int32_t, int32_t>> vals) {
  std::vector<uint8_t> raw(vals.size() * 8);
  for (size_t i = 0; i < vals.size(); ++i) {
    EncodeU32(std::bit_cast<uint32_t>(vals[i].first), b.big_endian(),
              raw.data() + i * 8);
    EncodeU32(std::bit_cast<uint32_t>(vals[i].second), b.big_endian(),
              raw.data() + i * 8 + 4);
  }
  return MakeBytesEntry(b, tag, TIFF_SRATIONAL, vals.size(), std::move(raw));
}

// Appends base required tags for a valid minimal single-strip uncompressed
// 4x4 grayscale image unless caller omits specific tags for negative tests.
std::vector<RawIfdEntry> MakeBaseImageEntries(RawTiffBuilder& b,
                                              uint32_t width = 4,
                                              uint32_t height = 4,
                                              uint16_t bps = 8,
                                              uint16_t spp = 1) {
  const size_t strip_bytes =
      static_cast<size_t>(width) * height * spp * ((bps + 7) / 8);
  std::vector<uint8_t> pixels(strip_bytes, 0x2a);
  const uint64_t strip_off = b.AppendBlob(pixels);
  std::vector<RawIfdEntry> entries;
  entries.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, width));
  entries.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, height));
  entries.push_back(b.MakeShort(TIFFTAG_BITSPERSAMPLE, bps));
  entries.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
  entries.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
  if (b.big_tiff()) {
    entries.push_back(b.MakeLong8(TIFFTAG_STRIPOFFSETS, strip_off));
    entries.push_back(b.MakeLong8(TIFFTAG_STRIPBYTECOUNTS, strip_bytes));
  } else {
    entries.push_back(
        b.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(strip_off)));
    entries.push_back(b.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                                 static_cast<uint32_t>(strip_bytes)));
  }
  entries.push_back(b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, spp));
  entries.push_back(b.MakeLong(TIFFTAG_ROWSPERSTRIP, height));
  return entries;
}

// Custom tag numbers far outside standard TIFF / DNG / EXIF ranges.
enum TestCustomTag : ttag_t {
  kTagSint8 = 65201,
  kTagSint16 = 65202,
  kTagSint32 = 65203,
  kTagSint64 = 65204,
  kTagC0Sint8 = 65205,
  kTagC0Sint16 = 65206,
  kTagC0Sint32 = 65207,
  kTagC0Uint64 = 65208,
  kTagC0Sint64 = 65209,
  kTagC0Double = 65210,
  kTagC0Ifd8 = 65211,
  kTagC16Sint8 = 65212,
  kTagC16Sint16 = 65213,
  kTagC16Sint32 = 65214,
  kTagC16Uint64 = 65215,
  kTagC16Sint64 = 65216,
  kTagC16Double = 65217,
  kTagC32Ascii = 65218,
  kTagUndefinedType = 65219,
  kTagIgnoredCustom = 65220,
  kTagC16Float = 65221,
  kTagC16Ifd8 = 65222,
  kTagUint8 = 65223,
  kTagUint16 = 65224,
  kTagUint32 = 65225,
  kTagUint64 = 65226,
  kTagFloat = 65227,
  kTagDouble = 65228,
  kTagIfd8 = 65229,
  kTagC16Ascii = 65230,
  kTagC16Uint8 = 65231,
  kTagC16Uint16 = 65232,
  kTagC16Uint32 = 65233,
  kTagC32Uint8 = 65234,
  kTagC32Uint16 = 65235,
  kTagC32Sint32 = 65236,
  kTagC32Double = 65237,
  kTagC32Ifd8 = 65238,
  kTagC0Uint8 = 65239,
  kTagC0Uint16 = 65240,
  kTagC0Uint32 = 65241,
  kTagC0Float = 65242,
};

const TIFFFieldInfo kCustomTestFieldInfos[] = {
    {kTagSint8, 1, 1, TIFF_SBYTE, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestSint8")},
    {kTagSint16, 1, 1, TIFF_SSHORT, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestSint16")},
    {kTagSint32, 1, 1, TIFF_SLONG, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestSint32")},
    {kTagSint64, 1, 1, TIFF_SLONG8, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestSint64")},
    {kTagC0Sint8, 3, 3, TIFF_SBYTE, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestC0Sint8")},
    {kTagC0Sint16, 2, 2, TIFF_SSHORT, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestC0Sint16")},
    {kTagC0Sint32, 2, 2, TIFF_SLONG, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestC0Sint32")},
    {kTagC0Uint64, 2, 2, TIFF_LONG8, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestC0Uint64")},
    {kTagC0Sint64, 2, 2, TIFF_SLONG8, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestC0Sint64")},
    {kTagC0Double, 2, 2, TIFF_DOUBLE, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestC0Double")},
    {kTagC0Ifd8, 2, 2, TIFF_IFD8, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestC0Ifd8")},
    {kTagC16Sint8, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_SBYTE, FIELD_CUSTOM, 1, 1,
     const_cast<char*>("TestC16Sint8")},
    {kTagC16Sint16, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_SSHORT, FIELD_CUSTOM, 1,
     1, const_cast<char*>("TestC16Sint16")},
    {kTagC16Sint32, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_SLONG, FIELD_CUSTOM, 1,
     1, const_cast<char*>("TestC16Sint32")},
    {kTagC16Uint64, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_LONG8, FIELD_CUSTOM, 1,
     1, const_cast<char*>("TestC16Uint64")},
    {kTagC16Sint64, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_SLONG8, FIELD_CUSTOM, 1,
     1, const_cast<char*>("TestC16Sint64")},
    {kTagC16Double, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_DOUBLE, FIELD_CUSTOM, 1,
     1, const_cast<char*>("TestC16Double")},
    {kTagC32Ascii, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_ASCII, FIELD_CUSTOM, 1,
     1, const_cast<char*>("TestC32Ascii")},
    {kTagUndefinedType, 1, 1, TIFF_NOTYPE, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestUndefinedType")},
    {kTagIgnoredCustom, 0, 0, TIFF_LONG, FIELD_IGNORE, 0, 0,
     const_cast<char*>("TestIgnoredCustom")},
    {kTagC16Float, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_FLOAT, FIELD_CUSTOM, 1, 1,
     const_cast<char*>("TestC16Float")},
    {kTagC16Ifd8, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_IFD8, FIELD_CUSTOM, 1, 1,
     const_cast<char*>("TestC16Ifd8")},
    {kTagUint8, 1, 1, TIFF_BYTE, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestUint8")},
    {kTagUint16, 1, 1, TIFF_SHORT, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestUint16")},
    {kTagUint32, 1, 1, TIFF_LONG, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestUint32")},
    {kTagUint64, 1, 1, TIFF_LONG8, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestUint64")},
    {kTagFloat, 1, 1, TIFF_FLOAT, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestFloat")},
    {kTagDouble, 1, 1, TIFF_DOUBLE, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestDouble")},
    {kTagIfd8, 1, 1, TIFF_IFD8, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestIfd8")},
    {kTagC16Ascii, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_ASCII, FIELD_CUSTOM, 1, 1,
     const_cast<char*>("TestC16Ascii")},
    {kTagC16Uint8, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_BYTE, FIELD_CUSTOM, 1, 1,
     const_cast<char*>("TestC16Uint8")},
    {kTagC16Uint16, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_SHORT, FIELD_CUSTOM, 1,
     1, const_cast<char*>("TestC16Uint16")},
    {kTagC16Uint32, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_LONG, FIELD_CUSTOM, 1, 1,
     const_cast<char*>("TestC16Uint32")},
    {kTagC32Uint8, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_BYTE, FIELD_CUSTOM, 1,
     1, const_cast<char*>("TestC32Uint8")},
    {kTagC32Uint16, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_SHORT, FIELD_CUSTOM, 1,
     1, const_cast<char*>("TestC32Uint16")},
    {kTagC32Sint32, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_SLONG, FIELD_CUSTOM, 1,
     1, const_cast<char*>("TestC32Sint32")},
    {kTagC32Double, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE, FIELD_CUSTOM,
     1, 1, const_cast<char*>("TestC32Double")},
    {kTagC32Ifd8, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_IFD8, FIELD_CUSTOM, 1, 1,
     const_cast<char*>("TestC32Ifd8")},
    {kTagC0Uint8, 2, 2, TIFF_BYTE, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestC0Uint8")},
    {kTagC0Uint16, 2, 2, TIFF_SHORT, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestC0Uint16")},
    {kTagC0Uint32, 2, 2, TIFF_LONG, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestC0Uint32")},
    {kTagC0Float, 2, 2, TIFF_FLOAT, FIELD_CUSTOM, 1, 0,
     const_cast<char*>("TestC0Float")},
};

void RegisterCustomTestTags(TIFF* tif) {
  TIFFMergeFieldInfo(tif, kCustomTestFieldInfos,
                     static_cast<uint32_t>(std::size(kCustomTestFieldInfos)));
}

// Global error capture helper for paths invoking TIFFErrorExt directly
// (such as the Google patch on unmapped dircount == 0).
std::string& GlobalExtErrorMessage() {
  static auto* msg = new std::string();
  return *msg;
}

void CaptureGlobalExtError(thandle_t /*fd*/, const char* module,
                           const char* fmt, va_list ap) {
  char buf[512];
  std::vsnprintf(buf, sizeof(buf), fmt != nullptr ? fmt : "", ap);
  GlobalExtErrorMessage() =
      std::string(module != nullptr ? module : "") + ": " + buf;
}

class TifDirReadFormatTest : public ::testing::TestWithParam<FormatParam> {};

TEST_P(TifDirReadFormatTest, ReadsBasicDirectoryAcrossFormats) {
  const auto p = GetParam();
  RawTiffBuilder b(p.big_endian, p.big_tiff);
  auto entries = MakeBaseImageEntries(b, 8, 6, 8, 1);
  entries.push_back(b.MakeAscii(TIFFTAG_IMAGEDESCRIPTION, "hello"));
  b.AddIfd(std::move(entries));

  MemoryTiffStream stream(b.Take());
  stream.set_enable_mmap(p.mmap);
  UniqueTiff tif = stream.Open(ReadMode(p.mmap));
  ASSERT_NE(tif, nullptr);

  uint32_t w = 0;
  uint32_t h = 0;
  uint16_t bps = 0;
  const char* desc = nullptr;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &w), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGELENGTH, &h), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_BITSPERSAMPLE, &bps), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEDESCRIPTION, &desc), 1);
  EXPECT_EQ(w, 8U);
  EXPECT_EQ(h, 6U);
  EXPECT_EQ(bps, 8U);
  EXPECT_STREQ(desc, "hello");
  EXPECT_EQ(TIFFReadDirectory(tif.get()), 0);  // nextdiroff == 0
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST_P(TifDirReadFormatTest, RejectsZeroDirectoryCountCleanly) {
  const auto p = GetParam();
  RawTiffBuilder b(p.big_endian, p.big_tiff);
  // Empty IFD -> dircount == 0.
  b.AddIfd({});

  GlobalExtErrorMessage().clear();
  ScopedTiffErrorHandlers err_guard(nullptr, nullptr);
  ScopedTiffErrorHandlersExt ext_guard(&CaptureGlobalExtError, nullptr);

  MemoryTiffStream stream(b.Take());
  stream.set_enable_mmap(p.mmap);
  UniqueTiff tif = stream.Open(HeaderOnlyMode(p.mmap));
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(TIFFReadDirectory(tif.get()), 0);
  if (p.mmap) {
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
    EXPECT_THAT(stream.diagnostics().errors()[0].message,
                HasSubstr("zero tag directories not supported"));
  } else {
    EXPECT_THAT(GlobalExtErrorMessage(),
                HasSubstr("Invalid zero directory count"));
  }
}

TEST_P(TifDirReadFormatTest, RejectsTooLargeDirectoryCount) {
  const auto p = GetParam();
  RawTiffBuilder b(p.big_endian, p.big_tiff);
  // Write a fake IFD with dircount = 4097 (> 4096).
  const uint64_t off = b.bytes().size();
  b.PatchHeaderFirstIfdOffset(off);
  std::vector<uint8_t> buf(b.big_tiff() ? 8 : 2);
  if (b.big_tiff()) {
    EncodeU64(4097, b.big_endian(), buf.data());
  } else {
    EncodeU16(4097, b.big_endian(), buf.data());
  }
  b.mutable_bytes().insert(b.mutable_bytes().end(), buf.begin(), buf.end());

  MemoryTiffStream stream(b.Take());
  stream.set_enable_mmap(p.mmap);
  UniqueTiff tif = stream.Open(HeaderOnlyMode(p.mmap));
  ASSERT_NE(tif, nullptr);
  EXPECT_EQ(TIFFReadDirectory(tif.get()), 0);
  EXPECT_GE(stream.diagnostics().error_count(), 1U);
  EXPECT_THAT(stream.diagnostics().errors()[0].message,
              HasSubstr("Sanity check on directory count failed"));
}

TEST_P(TifDirReadFormatTest, HandlesTruncatedDirectoryAtVariousStages) {
  const auto p = GetParam();
  // Stage 1: header points past EOF -> cannot read directory count.
  {
    RawTiffBuilder b(p.big_endian, p.big_tiff);
    b.PatchHeaderFirstIfdOffset(512);
    MemoryTiffStream stream(b.Take());
    stream.set_enable_mmap(p.mmap);
    UniqueTiff tif = stream.Open(HeaderOnlyMode(p.mmap));
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFReadDirectory(tif.get()), 0);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }
  // Stage 2: valid dircount=1 (or 200 for unmapped filesize check) but
  // truncated entry bytes.
  {
    RawTiffBuilder b(p.big_endian, p.big_tiff);
    const uint64_t off = b.bytes().size();
    b.PatchHeaderFirstIfdOffset(off);
    std::vector<uint8_t> hdr(b.big_tiff() ? 8 : 2);
    if (b.big_tiff()) {
      EncodeU64(50, b.big_endian(), hdr.data());
    } else {
      EncodeU16(50, b.big_endian(), hdr.data());
    }
    b.mutable_bytes().insert(b.mutable_bytes().end(), hdr.begin(), hdr.end());
    b.mutable_bytes().push_back(0);  // Short by many bytes.

    MemoryTiffStream stream(b.Take());
    stream.set_enable_mmap(p.mmap);
    UniqueTiff tif = stream.Open(HeaderOnlyMode(p.mmap));
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFReadDirectory(tif.get()), 0);
  }
}

TEST_P(TifDirReadFormatTest, EvaluatesAllScalarCoercionsAndRangeChecks) {
  const auto p = GetParam();
  ScopedTagExtender extender(&RegisterCustomTestTags);

  // Part A: Exercise valid scalar type coercions into UINT8, SINT8, UINT16,
  // SINT16, UINT32, SINT32, UINT64, SINT64, FLOAT, DOUBLE, IFD8, ASCII.
  RawTiffBuilder b(p.big_endian, p.big_tiff);
  auto entries = MakeBaseImageEntries(b);
  // TIFF_SETGET_UINT8 (TIFFTAG_FAXPROFILE): feed from SBYTE, SHORT, SSHORT,
  // LONG, SLONG, LONG8, SLONG8, UNDEFINED.
  entries.push_back(b.MakeByte(TIFFTAG_FAXPROFILE, 7, TIFF_UNDEFINED));
  entries.push_back(MakeSbyteEntry(b, kTagSint8, -12));
  entries.push_back(b.MakeByte(TIFFTAG_INDEXED, 1, TIFF_BYTE));
  entries.push_back(MakeSshortEntry(b, kTagSint16, -320));
  entries.push_back(
      b.MakeLong8(TIFFTAG_PROFILETYPE, 42U, TIFF_LONG8));      // LONG8->UINT32
  entries.push_back(b.MakeLong(kTagSint32, 1234, TIFF_LONG));  // LONG->SLONG
  entries.push_back(b.MakeLong(TIFFTAG_EXIFIFD, 256, TIFF_IFD));  // IFD->UINT64
  entries.push_back(
      MakeSlong8Entry(b, kTagSint64, -999999));  // SLONG8->SINT64 (BigTIFF)
  entries.push_back(b.MakeSrational(TIFFTAG_XRESOLUTION, 300, 10));  // FLOAT
  entries.push_back(b.MakeRational(TIFFTAG_YRESOLUTION, 72, 0));  // denom == 0
  entries.push_back(b.MakeDouble(TIFFTAG_XPOSITION, 1.5));  // DOUBLE->FLOAT
  entries.push_back(
      b.MakeSrational(TIFFTAG_STONITS, -15, 2));  // SRATIONAL->DOUBLE
  entries.push_back(
      b.MakeLong(TIFFTAG_INTEROPERABILITYIFD, 512, TIFF_LONG));  // LONG->IFD8
  // Out-of-line & NUL-missing & embedded-NUL ASCII tests.
  entries.push_back(
      b.MakeAscii(TIFFTAG_DOCUMENTNAME, "no_nul", /*append_nul=*/false));
  entries.push_back(
      b.MakeAscii(TIFFTAG_IMAGEDESCRIPTION, std::string_view("a\0b", 3), true));
  b.AddIfd(std::move(entries));

  MemoryTiffStream stream(b.Take());
  stream.set_enable_mmap(p.mmap);
  UniqueTiff tif = stream.Open(ReadMode(p.mmap));
  ASSERT_NE(tif, nullptr);

  uint8_t faxprofile = 0;
  int8_t s8 = 0;
  uint16_t indexed = 0;
  int16_t s16 = 0;
  uint32_t profiletype = 0;
  int32_t s32 = 0;
  uint64_t exif_off = 0;
  int64_t s64 = 0;
  float xres = 0.0f;
  float yres = 1.0f;
  float xpos = 0.0f;
  double stonits = 0.0;
  uint64_t interop = 0;
  const char* docname = nullptr;
  const char* desc = nullptr;

  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_FAXPROFILE, &faxprofile), 1);
  EXPECT_EQ(faxprofile, 7);
  EXPECT_EQ(TIFFGetField(tif.get(), kTagSint8, &s8), 1);
  EXPECT_EQ(s8, -12);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_INDEXED, &indexed), 1);
  EXPECT_EQ(indexed, 1U);
  EXPECT_EQ(TIFFGetField(tif.get(), kTagSint16, &s16), 1);
  EXPECT_EQ(s16, -320);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PROFILETYPE, &profiletype), 1);
  EXPECT_EQ(profiletype, 42U);
  EXPECT_EQ(TIFFGetField(tif.get(), kTagSint32, &s32), 1);
  EXPECT_EQ(s32, 1234);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_EXIFIFD, &exif_off), 1);
  EXPECT_EQ(exif_off, 256U);
  EXPECT_EQ(TIFFGetField(tif.get(), kTagSint64, &s64), 1);
  EXPECT_EQ(s64, -999999);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_XRESOLUTION, &xres), 1);
  EXPECT_FLOAT_EQ(xres, 30.0f);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_YRESOLUTION, &yres), 1);
  EXPECT_FLOAT_EQ(yres, 0.0f);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_XPOSITION, &xpos), 1);
  EXPECT_FLOAT_EQ(xpos, 1.5f);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_STONITS, &stonits), 1);
  EXPECT_DOUBLE_EQ(stonits, -7.5);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_INTEROPERABILITYIFD, &interop), 1);
  EXPECT_EQ(interop, 512U);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_DOCUMENTNAME, &docname), 1);
  EXPECT_STREQ(docname, "no_nul");
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEDESCRIPTION, &desc), 1);
  EXPECT_STREQ(desc, "a");
  // Expect warnings for forced NUL and embedded NUL.
  EXPECT_GE(stream.diagnostics().warning_count(), 2U);
}

INSTANTIATE_TEST_SUITE_P(Formats, TifDirReadFormatTest,
                         ::testing::ValuesIn(kFormatParams),
                         [](const ::testing::TestParamInfo<FormatParam>& info) {
                           return info.param.name;
                         });

// Exhaustive matrix for scalar ReadDirEntry* coercions, range errors, type
// errors, count errors, and I/O errors (both Classic and BigTIFF, LE and BE).
TEST(TifDirReadTest, ScalarCoercionsAndRangeErrorsComprehensive) {
  ScopedTagExtender extender(&RegisterCustomTestTags);

  struct Case {
    RawIfdEntry entry;
    ttag_t tag;
    bool expect_present;
  };

  for (bool be : {false, true}) {
    for (bool big : {false, true}) {
      // Valid alternate source coercions into every scalar target type.
      RawTiffBuilder b(be, big);
      auto entries = MakeBaseImageEntries(b);
      // UINT8 <- SBYTE / SHORT / SSHORT / LONG / SLONG / LONG8 / SLONG8
      entries.push_back(MakeSbyteEntry(b, TIFFTAG_FAXPROFILE, 12));
      entries.push_back(b.MakeLong8(TIFFTAG_MODENUMBER, 200, TIFF_LONG8));
      // SINT8 <- BYTE(positive), SHORT, SSHORT, LONG, SLONG, LONG8, SLONG8
      entries.push_back(b.MakeLong8(kTagSint8, 100, TIFF_LONG8));
      // UINT16 <- BYTE, SBYTE, SSHORT, LONG, SLONG, LONG8, SLONG8
      entries.push_back(MakeSlong8Entry(b, TIFFTAG_CELLWIDTH, 64));
      entries.push_back(b.MakeLong8(TIFFTAG_CELLLENGTH, 32, TIFF_LONG8));
      entries.push_back(MakeSbyteEntry(b, TIFFTAG_INDEXED, 3));
      // SINT16 <- BYTE, SBYTE, SHORT, LONG, SLONG, LONG8, SLONG8
      entries.push_back(b.MakeLong8(kTagSint16, 30000, TIFF_LONG8));
      // UINT32 <- BYTE, SBYTE, SHORT, SSHORT, SLONG, SLONG8
      entries.push_back(MakeSbyteEntry(b, TIFFTAG_PROFILETYPE, 9));
      entries.push_back(MakeSlong8Entry(b, TIFFTAG_CODINGMETHODS, 999));
      // SINT32 <- BYTE, SBYTE, SHORT, SSHORT, LONG8, SLONG8
      entries.push_back(MakeSlong8Entry(b, kTagSint32, -123456));
      // UINT64 <- BYTE, SBYTE, SHORT, SSHORT, SLONG, SLONG8, LONG8
      entries.push_back(MakeSlong8Entry(b, TIFFTAG_EXIFIFD, 4096));
      // SINT64 <- BYTE, SBYTE, SHORT, SSHORT, LONG, SLONG, LONG8
      entries.push_back(b.MakeLong8(kTagSint64, 70000, TIFF_LONG8));
      // FLOAT <- BYTE, SBYTE, SHORT, SSHORT, LONG, SLONG, LONG8, SLONG8,
      // RATIONAL
      entries.push_back(b.MakeLong8(TIFFTAG_XRESOLUTION, 150, TIFF_LONG8));
      entries.push_back(MakeSlong8Entry(b, TIFFTAG_YRESOLUTION, 75));
      entries.push_back(MakeSbyteEntry(b, TIFFTAG_PIXAR_FOVCOT, -5));
      entries.push_back(
          b.MakeSrational(TIFFTAG_XPOSITION, 0, 5));  // 0 numerator
      entries.push_back(b.MakeSrational(TIFFTAG_YPOSITION, 10, 0));  // 0 denom
      // DOUBLE <- BYTE, SBYTE, SHORT, SSHORT, LONG, SLONG, LONG8, SLONG8,
      // FLOAT, RATIONAL
      entries.push_back(MakeSlong8Entry(b, TIFFTAG_STONITS, -888));
      // IFD8 <- IFD, LONG8, IFD8
      entries.push_back(
          b.MakeLong8(TIFFTAG_INTEROPERABILITYIFD, 777, TIFF_IFD8));
      // Custom TIFF_SETGET_UNDEFINED tag
      entries.push_back(b.MakeLong(kTagUndefinedType, 1, TIFF_NOTYPE));
      // FIELD_IGNORE custom tag
      entries.push_back(b.MakeLong(kTagIgnoredCustom, 999, TIFF_LONG));
      // PageNumber UINT16_PAIR
      entries.push_back(
          b.MakeShortArray(TIFFTAG_PAGENUMBER, std::array<uint16_t, 2>{1, 5}));
      b.AddIfd(std::move(entries));

      MemoryTiffStream stream(b.Take());
      UniqueTiff tif = stream.Open("rm");
      ASSERT_NE(tif, nullptr);

      uint8_t u8 = 0;
      int8_t s8 = 0;
      uint16_t u16 = 0;
      int16_t s16 = 0;
      uint32_t u32 = 0;
      int32_t s32 = 0;
      uint64_t u64 = 0;
      int64_t s64 = 0;
      float fval = 0.0f;
      double dval = 0.0;
      uint16_t p0 = 0;
      uint16_t p1 = 0;
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_FAXPROFILE, &u8), 1);
      EXPECT_EQ(u8, 12);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_MODENUMBER, &u8), 1);
      EXPECT_EQ(u8, 200);
      EXPECT_EQ(TIFFGetField(tif.get(), kTagSint8, &s8), 1);
      EXPECT_EQ(s8, 100);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_CELLWIDTH, &u16), 1);
      EXPECT_EQ(u16, 64U);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_CELLLENGTH, &u16), 1);
      EXPECT_EQ(u16, 32U);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_INDEXED, &u16), 1);
      EXPECT_EQ(u16, 3U);
      EXPECT_EQ(TIFFGetField(tif.get(), kTagSint16, &s16), 1);
      EXPECT_EQ(s16, 30000);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PROFILETYPE, &u32), 1);
      EXPECT_EQ(u32, 9U);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_CODINGMETHODS, &u32), 1);
      EXPECT_EQ(u32, 999U);
      EXPECT_EQ(TIFFGetField(tif.get(), kTagSint32, &s32), 1);
      EXPECT_EQ(s32, -123456);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_EXIFIFD, &u64), 1);
      EXPECT_EQ(u64, 4096U);
      EXPECT_EQ(TIFFGetField(tif.get(), kTagSint64, &s64), 1);
      EXPECT_EQ(s64, 70000);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_XRESOLUTION, &fval), 1);
      EXPECT_FLOAT_EQ(fval, 150.0f);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_YRESOLUTION, &fval), 1);
      EXPECT_FLOAT_EQ(fval, 75.0f);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PIXAR_FOVCOT, &fval), 1);
      EXPECT_FLOAT_EQ(fval, -5.0f);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_XPOSITION, &fval), 1);
      EXPECT_FLOAT_EQ(fval, 0.0f);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_YPOSITION, &fval), 1);
      EXPECT_FLOAT_EQ(fval, 0.0f);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_STONITS, &dval), 1);
      EXPECT_DOUBLE_EQ(dval, -888.0);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_INTEROPERABILITYIFD, &u64), 1);
      EXPECT_EQ(u64, 777U);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PAGENUMBER, &p0, &p1), 1);
      EXPECT_EQ(p0, 1U);
      EXPECT_EQ(p1, 5U);
      // Ignored custom tag was skipped cleanly; UndefinedType emitted an error
      // (without failing recover=1 directory load).
      EXPECT_EQ(TIFFGetField(tif.get(), kTagIgnoredCustom, &u32), 0);
      EXPECT_GE(stream.diagnostics().error_count(), 1U);
    }

    // Out-of-range, wrong-type, and wrong-count paths across scalar readers.
    for (bool big : {false, true}) {
      RawTiffBuilder b(be, big);
      auto entries = MakeBaseImageEntries(b);
      entries.push_back(MakeSbyteEntry(b, TIFFTAG_FAXPROFILE, -1));  // range
      entries.push_back(b.MakeByte(TIFFTAG_MODENUMBER, 1, TIFF_FLOAT));  // type
      entries.push_back(
          b.MakeByte(kTagSint8, 200, TIFF_BYTE));  // > 127 range for SBYTE
      entries.push_back(
          b.MakeLong(TIFFTAG_CELLWIDTH, 70000));  // > 65535 range for SHORT
      entries.push_back(
          MakeSshortEntry(b, TIFFTAG_CELLLENGTH, -1));  // < 0 range for SHORT
      entries.push_back(b.MakeLong8(TIFFTAG_INDEXED, 70000, TIFF_LONG8));
      entries.push_back(
          MakeSlong8Entry(b, TIFFTAG_ORIENTATION, -5));  // < 0 SLONG8->SHORT
      entries.push_back(
          b.MakeShort(kTagSint16, 40000));  // > 32767 range for SSHORT
      entries.push_back(b.MakeLong(kTagSint32, 0x90000000U));  // > INT32_MAX
      entries.push_back(MakeSbyteEntry(b, TIFFTAG_PROFILETYPE, -2));  // < 0
      entries.push_back(
          b.MakeLong8(TIFFTAG_CODINGMETHODS, 0x100000000ULL, TIFF_LONG8));
      entries.push_back(
          MakeSbyteEntry(b, TIFFTAG_EXIFIFD, -3));  // < 0 for UINT64
      entries.push_back(b.MakeLong8(kTagSint64, 0x9000000000000000ULL,
                                    TIFF_LONG8));  // > INT64_MAX for SLONG8
      entries.push_back(
          b.MakeDouble(TIFFTAG_XRESOLUTION, 1e100));  // > FLT_MAX for FLOAT
      entries.push_back(
          b.MakeDouble(TIFFTAG_YRESOLUTION, -1e100));  // < -FLT_MAX for FLOAT
      entries.push_back(
          b.MakeByte(TIFFTAG_PIXAR_FOVCOT, 1, TIFF_ASCII));  // bad type Float
      entries.push_back(
          b.MakeByte(TIFFTAG_STONITS, 1, TIFF_ASCII));  // bad type Double
      entries.push_back(
          b.MakeByte(TIFFTAG_INTEROPERABILITYIFD, 1, TIFF_ASCII));  // bad IFD8
      entries.push_back(
          b.MakeShort(TIFFTAG_PAGENUMBER, 1));  // count!=2 for UINT16_PAIR
      entries.push_back(
          b.MakeShortArray(TIFFTAG_RESOLUTIONUNIT,
                           std::array<uint16_t, 2>{1, 2}));  // count!=1 Short
      b.AddIfd(std::move(entries));

      MemoryTiffStream stream(b.Take());
      UniqueTiff tif = stream.Open("rm");
      ASSERT_NE(tif, nullptr);
      // Recoverable errors ignore bad optional tags and still open directory.
      EXPECT_GE(stream.diagnostics().error_count() +
                    stream.diagnostics().warning_count(),
                10U);
      uint8_t u8 = 0;
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_FAXPROFILE, &u8), 0);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_MODENUMBER, &u8), 0);
    }
  }
}

// Verify array-based coercions (C0_*, C16_*, C32_*), swap, OOM on secondary
// array conversion buffer, and unknown/anonymous tag creation + lookup.
TEST(TifDirReadTest, ArrayCoercionsAnonymousTagsAndConversionErrors) {
  ScopedTagExtender extender(&RegisterCustomTestTags);

  for (bool be : {false, true}) {
    for (bool big : {false, true}) {
      RawTiffBuilder b(be, big);
      auto entries = MakeBaseImageEntries(b, 4, 4, 8, 2);
      entries.push_back(b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG));

      // C0_UINT8 (DNGVersion count 4) fed as SHORT array -> triggers
      // conversion.
      entries.push_back(b.MakeShortArray(TIFFTAG_DNGVERSION,
                                         std::array<uint16_t, 4>{1, 4, 0, 0}));
      // C0_SINT8 (count 3)
      entries.push_back(MakeSshortArrayEntry(b, kTagC0Sint8,
                                             std::array<int16_t, 3>{-5, 0, 7}));
      // C0_UINT16 (BlackLevelRepeatDim count 2) fed as LONG
      entries.push_back(b.MakeLongArray(TIFFTAG_BLACKLEVELREPEATDIM,
                                        std::array<uint32_t, 2>{2, 2}));
      // C0_SINT16 (count 2) fed as SLONG
      entries.push_back(MakeSlongArrayEntry(b, kTagC0Sint16,
                                            std::array<int32_t, 2>{-100, 200}));
      // C0_UINT32 (ActiveArea count 4) fed as SHORT
      entries.push_back(b.MakeShortArray(TIFFTAG_ACTIVEAREA,
                                         std::array<uint16_t, 4>{0, 0, 4, 4}));
      // C0_SINT32 (count 2) fed as LONG8
      entries.push_back(MakeSlong8ArrayEntry(b, kTagC0Sint32,
                                             std::array<int64_t, 2>{-10, 20}));
      // C0_UINT64 (count 2) fed as LONG (note: values fit 32-bit for
      // ClassicTIFF)
      entries.push_back(
          b.MakeLongArray(kTagC0Uint64, std::array<uint32_t, 2>{11, 22}));
      // C0_SINT64 (count 2) fed as SBYTE
      entries.push_back(
          MakeSbyteArrayEntry(b, kTagC0Sint64, std::array<int8_t, 2>{-3, 9}));
      // C0_FLOAT (WhitePoint count 2) fed as DOUBLE
      entries.push_back(MakeDoubleArrayEntry(
          b, TIFFTAG_WHITEPOINT, std::array<double, 2>{0.3127, 0.329}));
      // C0_DOUBLE (count 2) fed as SRATIONAL
      entries.push_back(MakeSrationalArrayEntry(
          b, kTagC0Double,
          std::array<std::pair<int32_t, int32_t>, 2>{{{-3, 2}, {5, 0}}}));
      // C0_IFD8 (count 2) fed as IFD
      entries.push_back(b.MakeLongArray(
          kTagC0Ifd8, std::array<uint32_t, 2>{128, 256}, TIFF_IFD));

      // C16_* arrays
      entries.push_back(b.MakeAscii(TIFFTAG_INKNAMES,
                                    std::string_view("Cyan\0Magenta\0", 13)));
      entries.push_back(b.MakeByte(TIFFTAG_CFAPLANECOLOR, 1, TIFF_BYTE));
      entries.push_back(
          MakeSbyteArrayEntry(b, kTagC16Sint8, std::array<int8_t, 2>{-1, 2}));
      entries.push_back(b.MakeShortArray(TIFFTAG_LINEARIZATIONTABLE,
                                         std::array<uint16_t, 3>{10, 20, 30}));
      entries.push_back(MakeSshortArrayEntry(
          b, kTagC16Sint16, std::array<int16_t, 2>{-300, 300}));
      entries.push_back(b.MakeLongArray(TIFFTAG_WHITELEVEL,
                                        std::array<uint32_t, 2>{255, 255}));
      entries.push_back(MakeSlongArrayEntry(
          b, kTagC16Sint32, std::array<int32_t, 2>{-70000, 70000}));
      entries.push_back(
          b.MakeLong8Array(kTagC16Uint64, std::array<uint64_t, 2>{5, 6}));
      entries.push_back(MakeSlong8ArrayEntry(b, kTagC16Sint64,
                                             std::array<int64_t, 2>{-7, 8}));
      entries.push_back(MakeFloatArrayEntry(
          b, kTagC16Float, std::array<float, 2>{1.25f, -2.5f}));
      entries.push_back(MakeDoubleArrayEntry(b, kTagC16Double,
                                             std::array<double, 2>{3.5, -4.5}));
      entries.push_back(
          b.MakeLongArray(kTagC16Ifd8, std::array<uint32_t, 2>{10, 20}));

      // C32_* arrays (explicit C32_ASCII + RichTIFFIPTC with TIFF_LONG +
      // unknown anonymous tags producing C32_SINT8..C32_IFD8).
      entries.push_back(
          b.MakeAscii(kTagC32Ascii, "nonul", /*append_nul=*/false));
      entries.push_back(
          b.MakeLongArray(TIFFTAG_RICHTIFFIPTC,
                          std::array<uint32_t, 2>{0x01020304U, 0x05060708U}));
      entries.push_back(
          MakeSbyteArrayEntry(b, 65301, std::array<int8_t, 2>{-11, 22}));
      entries.push_back(
          b.MakeShortArray(65302, std::array<uint16_t, 2>{111, 222}));
      entries.push_back(
          MakeSshortArrayEntry(b, 65303, std::array<int16_t, 2>{-111, 222}));
      entries.push_back(
          b.MakeLongArray(65304, std::array<uint32_t, 2>{1000, 2000}));
      entries.push_back(
          MakeSlongArrayEntry(b, 65305, std::array<int32_t, 2>{-1000, 2000}));
      entries.push_back(b.MakeLong8Array(65306, std::array<uint64_t, 2>{3, 4}));
      entries.push_back(
          MakeSlong8ArrayEntry(b, 65307, std::array<int64_t, 2>{-3, 4}));
      entries.push_back(
          MakeFloatArrayEntry(b, 65308, std::array<float, 2>{0.5f, -0.5f}));
      entries.push_back(
          MakeDoubleArrayEntry(b, 65309, std::array<double, 2>{9.5, -9.5}));
      entries.push_back(
          b.MakeLongArray(65310, std::array<uint32_t, 2>{50, 60}, TIFF_IFD));

      b.AddIfd(std::move(entries));

      DiagnosticRecorder diag;
      UniqueTiffOptions opts = MakeUniqueTiffOptions();
      diag.Install(opts.get());
      TIFFOpenOptionsSetWarnAboutUnknownTags(opts.get(), 1);
      MemoryTiffStream stream(b.Take());
      UniqueTiff tif = stream.Open("rm", opts.get());
      ASSERT_NE(tif, nullptr);

      // Verify C0/C16/C32 fields populated accurately.
      uint8_t* dng_ver = nullptr;
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_DNGVERSION, &dng_ver), 1);
      EXPECT_EQ(dng_ver[0], 1);
      EXPECT_EQ(dng_ver[1], 4);

      double* c0d = nullptr;
      EXPECT_EQ(TIFFGetField(tif.get(), kTagC0Double, &c0d), 1);
      EXPECT_DOUBLE_EQ(c0d[0], -1.5);
      EXPECT_DOUBLE_EQ(c0d[1], 0.0);

      uint16_t c16_n = 0;
      const int16_t* c16_s16 = nullptr;
      EXPECT_EQ(TIFFGetField(tif.get(), kTagC16Sint16, &c16_n, &c16_s16), 1);
      EXPECT_EQ(c16_n, 2U);
      EXPECT_EQ(c16_s16[0], -300);

      uint32_t c32_n = 0;
      const void* c32_ptr = nullptr;
      EXPECT_EQ(TIFFGetField(tif.get(), kTagC32Ascii, &c32_n, &c32_ptr), 1);
      EXPECT_EQ(c32_n, 6U);
      EXPECT_STREQ(static_cast<const char*>(c32_ptr), "nonul");

      const void* iptc_ptr = nullptr;
      EXPECT_EQ(
          TIFFGetField(tif.get(), TIFFTAG_RICHTIFFIPTC, &c32_n, &iptc_ptr), 1);
      EXPECT_EQ(c32_n, 8U);

      // Unknown tags emitted warnings because WarnAboutUnknownTags == 1.
      EXPECT_GE(diag.warning_count(), 10U);
      const void* anon_ptr = nullptr;
      EXPECT_EQ(TIFFGetField(tif.get(), 65305, &c32_n, &anon_ptr), 1);
      EXPECT_EQ(c32_n, 2U);
      EXPECT_EQ(static_cast<const int32_t*>(anon_ptr)[0], -1000);
    }
  }
}

TEST(TifDirReadTest, ExercisesAllArrayTypeSourceConversionsAndSwab) {
  ScopedTagExtender extender(&RegisterCustomTestTags);
  // Test every array target converter (ByteArray..Ifd8Array) with many source
  // types under both little-endian and big-endian BigTIFF so TIFF_SWAB paths in
  // every converter loop execute cleanly.
  for (bool be : {false, true}) {
    RawTiffBuilder b(be, /*big_tiff=*/true);
    auto entries = MakeBaseImageEntries(b);
    // DoubleArray <- BYTE, SBYTE, SHORT, SSHORT, LONG, SLONG, LONG8, SLONG8,
    // RATIONAL, SRATIONAL, FLOAT, DOUBLE
    entries.push_back(b.MakeShortArray(
        65320, std::array<uint16_t, 2>{1, 2}));  // -> C32_UINT16
    entries.push_back(
        b.MakeLong8Array(kTagC16Double, std::array<uint64_t, 2>{10, 20}));
    entries.push_back(MakeSrationalArrayEntry(
        b, 65321,
        std::array<std::pair<int32_t, int32_t>, 2>{{{-4, 2}, {9, 3}}}));
    // FloatArray <- SHORT, SSHORT, LONG, SLONG, LONG8, SLONG8, RATIONAL, DOUBLE
    entries.push_back(MakeDoubleArrayEntry(b, kTagC16Float,
                                           std::array<double, 2>{1.5, -2.5}));
    // ByteArray <- SBYTE, SHORT, SSHORT, LONG, SLONG, LONG8, SLONG8
    entries.push_back(
        b.MakeLongArray(TIFFTAG_CFAPLANECOLOR, std::array<uint32_t, 2>{1, 2}));
    entries.push_back(b.MakeLong8Array(TIFFTAG_DNGPRIVATEDATA,
                                       std::array<uint64_t, 2>{3, 4}));
    entries.push_back(MakeSbyteArrayEntry(b, TIFFTAG_LOCALIZEDCAMERAMODEL,
                                          std::array<int8_t, 2>{5, 6}));
    // SbyteArray <- BYTE, SHORT, SSHORT, LONG, SLONG, LONG8, SLONG8
    entries.push_back(
        b.MakeShortArray(kTagC16Sint8, std::array<uint16_t, 2>{1, 2}));
    entries.push_back(b.MakeLong8Array(65322, std::array<uint64_t, 2>{7, 8}));
    // ShortArray <- BYTE, SBYTE, SSHORT, LONG, SLONG, LONG8, SLONG8
    entries.push_back(MakeSshortArrayEntry(b, TIFFTAG_LINEARIZATIONTABLE,
                                           std::array<int16_t, 2>{10, 20}));
    entries.push_back(MakeSlong8ArrayEntry(b, TIFFTAG_IMAGEBASECOLOR,
                                           std::array<int64_t, 2>{30, 40}));
    // SshortArray <- BYTE, SBYTE, SHORT, LONG, SLONG, LONG8, SLONG8
    entries.push_back(
        b.MakeByte(kTagC16Sint16, 42, TIFF_BYTE));  // count=1 BYTE->SSHORT
    entries.push_back(b.MakeLong8Array(65323, std::array<uint64_t, 2>{50, 60}));
    // LongArray <- BYTE, SBYTE, SHORT, SSHORT, SLONG, LONG8, SLONG8
    entries.push_back(MakeSlongArrayEntry(b, TIFFTAG_WHITELEVEL,
                                          std::array<int32_t, 2>{250, 255}));
    entries.push_back(
        b.MakeLong8Array(TIFFTAG_MASKEDAREAS, std::array<uint64_t, 2>{1, 2}));
    // SlongArray <- BYTE, SBYTE, SHORT, SSHORT, LONG, LONG8, SLONG8
    entries.push_back(
        b.MakeLong8Array(kTagC16Sint32, std::array<uint64_t, 2>{100, 200}));
    // Long8Array <- BYTE, SBYTE, SHORT, SSHORT, LONG, SLONG, SLONG8
    entries.push_back(MakeSlongArrayEntry(b, kTagC16Uint64,
                                          std::array<int32_t, 2>{1000, 2000}));
    // Slong8Array <- BYTE, SBYTE, SHORT, SSHORT, LONG, SLONG, LONG8
    entries.push_back(
        b.MakeLongArray(kTagC16Sint64, std::array<uint32_t, 2>{11, 22}));
    // Ifd8Array <- LONG, LONG8, IFD, IFD8
    entries.push_back(b.MakeLong8Array(
        kTagC16Ifd8, std::array<uint64_t, 2>{100, 200}, TIFF_IFD8));
    b.AddIfd(std::move(entries));

    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);

    uint16_t cnt = 0;
    const double* dbl = nullptr;
    EXPECT_EQ(TIFFGetField(tif.get(), kTagC16Double, &cnt, &dbl), 1);
    EXPECT_EQ(cnt, 2U);
    EXPECT_DOUBLE_EQ(dbl[0], 10.0);

    const uint64_t* ifds = nullptr;
    EXPECT_EQ(TIFFGetField(tif.get(), kTagC16Ifd8, &cnt, &ifds), 1);
    EXPECT_EQ(ifds[1], 200U);
  }
}

TEST(TifDirReadTest, RejectsInvalidCountsTypesSizesAndHugeArrays) {
  ScopedTagExtender extender(&RegisterCustomTestTags);

  // Oversize counts > 0xFFFF for C16_*, wrong C0_* counts, MAX_SIZE_TAG_DATA
  // overflow (both typesize and desttypesize checks), and > 100MB sanity guard.
  RawTiffBuilder b(/*big_endian=*/false, /*big_tiff=*/false);
  auto entries = MakeBaseImageEntries(b);
  entries.push_back(b.MakeShort(TIFFTAG_DNGVERSION, 1));  // C0_UINT8 != 4
  entries.push_back(MakeSshortEntry(b, kTagC0Sint8, 1));  // C0_SINT8 != 3
  entries.push_back(b.MakeShort(TIFFTAG_BLACKLEVELREPEATDIM, 1));  // C0_U16 !=2
  entries.push_back(b.MakeShort(kTagC0Sint16, 1));                 // C0_S16 !=2
  entries.push_back(b.MakeLong(TIFFTAG_ACTIVEAREA, 1));            // C0_U32 !=4
  entries.push_back(b.MakeLong(kTagC0Sint32, 1));                  // C0_S32 !=2
  entries.push_back(b.MakeLong(kTagC0Uint64, 1));                  // C0_U64 !=2
  entries.push_back(b.MakeLong(kTagC0Sint64, 1));                  // C0_S64 !=2
  entries.push_back(b.MakeFloat(TIFFTAG_WHITEPOINT, 1.0f));        // C0_FLT !=2
  entries.push_back(b.MakeFloat(kTagC0Double, 1.0f));              // C0_DBL !=2
  entries.push_back(b.MakeLong(kTagC0Ifd8, 1));                    // C0_IFD8!=2

  // C16 count > 0xFFFF checks:
  entries.push_back(b.MakeRawInline(TIFFTAG_INKNAMES, TIFF_ASCII, 70000, 0));
  entries.push_back(
      b.MakeRawInline(TIFFTAG_CFAPLANECOLOR, TIFF_BYTE, 70000, 0));
  entries.push_back(b.MakeRawInline(kTagC16Sint8, TIFF_SBYTE, 70000, 0));
  entries.push_back(
      b.MakeRawInline(TIFFTAG_LINEARIZATIONTABLE, TIFF_SHORT, 70000, 0));
  entries.push_back(b.MakeRawInline(kTagC16Sint16, TIFF_SSHORT, 70000, 0));
  entries.push_back(b.MakeRawInline(TIFFTAG_WHITELEVEL, TIFF_LONG, 70000, 0));
  entries.push_back(b.MakeRawInline(kTagC16Sint32, TIFF_SLONG, 70000, 0));
  entries.push_back(b.MakeRawInline(kTagC16Uint64, TIFF_LONG8, 70000, 0));
  entries.push_back(b.MakeRawInline(kTagC16Sint64, TIFF_SLONG8, 70000, 0));
  entries.push_back(b.MakeRawInline(kTagC16Float, TIFF_FLOAT, 70000, 0));
  entries.push_back(b.MakeRawInline(kTagC16Double, TIFF_DOUBLE, 70000, 0));
  entries.push_back(b.MakeRawInline(kTagC16Ifd8, TIFF_IFD8, 70000, 0));

  // MAX_SIZE_TAG_DATA typesize check: 300M * 8 > 2 GiB
  entries.push_back(b.MakeRawInline(65330, TIFF_DOUBLE, 300000000U, 0));
  // MAX_SIZE_TAG_DATA desttypesize check: BYTE count 300M read into SLONG8 (8B)
  entries.push_back(b.MakeRawInline(kTagSint64, TIFF_BYTE, 300000000U, 0));
  // Datasize > 100MB & > filesize -> TIFFReadDirEntryErrAlloc
  entries.push_back(b.MakeRawInline(65331, TIFF_LONG, 30000000U, 0));
  // Zero count array -> TIFFReadDirEntryArray returns 0 / Ok cleanly
  entries.push_back(b.MakeRawInline(65332, TIFF_BYTE, 0, 0));
  // Incompatible type into ByteArray / LongArray / ShortArray / Ifd8Array
  entries.push_back(b.MakeAscii(TIFFTAG_CLIPPATH, "abc"));
  entries.push_back(b.MakeRawInline(65333, TIFF_ASCII, 2, 0));
  // Incompatible: ASCII -> LongArray / ShortArray / Ifd8Array
  entries.push_back(b.MakeAscii(TIFFTAG_MASKEDAREAS, "bad"));
  entries.push_back(b.MakeAscii(TIFFTAG_LINEARIZATIONTABLE, "bad"));
  entries.push_back(b.MakeAscii(TIFFTAG_SUBIFD, "bad"));

  b.AddIfd(std::move(entries));
  MemoryTiffStream stream(b.Take());
  UniqueTiff tif = stream.Open("rm");
  ASSERT_NE(tif, nullptr);
  EXPECT_GE(
      stream.diagnostics().warning_count() + stream.diagnostics().error_count(),
      20U);

  // Mapped filesize > tif_size guard inside TIFFReadDirEntryArrayWithLimit
  {
    RawTiffBuilder bm(false, false);
    auto em = MakeBaseImageEntries(bm);
    em.push_back(bm.MakeRawInline(65340, TIFF_BYTE, 50000, 0));
    bm.AddIfd(std::move(em));
    MemoryTiffStream mstream(bm.Take());
    mstream.set_enable_mmap(true);
    UniqueTiff mtif = mstream.Open("rM");
    ASSERT_NE(mtif, nullptr);
    EXPECT_GE(mstream.diagnostics().warning_count(), 1U);
  }
}

TEST(TifDirReadTest, ReadsLargeOutOfLineTagViaDataAndReallocAndHandlesFaults) {
  // > 1 MB payload forces multi-iteration realloc inside
  // TIFFReadDirEntryDataAndRealloc (INITIAL_THRESHOLD = 1048576).
  const size_t kLargeLen = 1100000;
  std::string large_text(kLargeLen - 1, 'A');
  large_text.push_back('\0');
  {
    RawTiffBuilder b(false, false);
    auto entries = MakeBaseImageEntries(b);
    entries.push_back(b.MakeOutOfLine(
        TIFFTAG_IMAGEDESCRIPTION, TIFF_ASCII, large_text.size(),
        std::vector<uint8_t>(large_text.begin(), large_text.end())));
    b.AddIfd(std::move(entries));

    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    const char* got = nullptr;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEDESCRIPTION, &got), 1);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(std::strlen(got), kLargeLen - 1);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }

  // OOM during TIFFReadDirEntryDataAndRealloc after directory loads under
  // header-only + ScopedCumulatedMemAllocRemaining.
  {
    RawTiffBuilder b(false, false);
    auto entries = MakeBaseImageEntries(b);
    entries.push_back(b.MakeOutOfLine(
        TIFFTAG_IMAGEDESCRIPTION, TIFF_ASCII, large_text.size(),
        std::vector<uint8_t>(large_text.begin(), large_text.end())));
    b.AddIfd(std::move(entries));

    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    diag.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rhm", opts.get());
    ASSERT_NE(tif, nullptr);
    {
      // Allow IFD (~150B) + offsets array (~200B), deny 1MB realloc.
      ScopedCumulatedMemAllocRemaining budget(tif.get(), 4096);
      EXPECT_EQ(TIFFReadDirectory(tif.get()), 1);  // ImageDescription ignored
    }
    EXPECT_GE(diag.error_count(), 1U);
    const char* got = nullptr;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEDESCRIPTION, &got), 0);
  }

  // IO short read on second chunk of TIFFReadDirEntryDataAndRealloc.
  {
    RawTiffBuilder b(false, false);
    auto entries = MakeBaseImageEntries(b);
    entries.push_back(b.MakeOutOfLine(
        TIFFTAG_IMAGEDESCRIPTION, TIFF_ASCII, large_text.size(),
        std::vector<uint8_t>(large_text.begin(), large_text.end())));
    b.AddIfd(std::move(entries));
    std::vector<uint8_t> bytes = b.Take();
    // Truncate by 100 bytes so first 1MB chunk succeeds, second fails short!
    bytes.resize(bytes.size() - 100);
    MemoryTiffStream stream(std::move(bytes));
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    const char* got = nullptr;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEDESCRIPTION, &got), 0);
    EXPECT_GE(stream.diagnostics().warning_count(), 1U);
  }
}

TEST(TifDirReadTest, DirectoryFixupsPersampleColormapExtraSamplesOjpeg) {
  // 1) ColorMap / TransferFunction fixups & edge cases.
  {
    // Valid BitsPerSample=2 ColorMap (3 * 4 = 12 shorts) + single-curve
    // TransferFunction (4 shorts) + OSUBFILETYPE PAGE + SMINSAMPLEVALUE.
    RawTiffBuilder b(false, false);
    auto entries = MakeBaseImageEntries(b, 4, 4, 2, 1);
    for (auto& e : entries) {
      if (e.tag == TIFFTAG_PHOTOMETRIC) {
        e = b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_PALETTE);
      }
      if (e.tag == TIFFTAG_STRIPBYTECOUNTS) {
        e = b.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 4);
      }
    }
    std::array<uint16_t, 12> cmap{};
    cmap.fill(1234);
    std::array<uint16_t, 4> tf{{10, 20, 30, 40}};
    entries.push_back(b.MakeShortArray(TIFFTAG_COLORMAP, cmap));
    entries.push_back(b.MakeShortArray(TIFFTAG_TRANSFERFUNCTION, tf));
    entries.push_back(b.MakeShort(TIFFTAG_OSUBFILETYPE, OFILETYPE_PAGE));
    entries.push_back(b.MakeDouble(TIFFTAG_SMINSAMPLEVALUE, 0.25));
    entries.push_back(b.MakeDouble(TIFFTAG_SMAXSAMPLEVALUE, 3.75));
    b.AddIfd(std::move(entries));

    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    // TIFFTAG_OSUBFILETYPE has FIELD_IGNORE in tif_dirinfo.c, so pass 1
    // ignores it and does not synthesize TIFFTAG_SUBFILETYPE.
    uint32_t subfile = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SUBFILETYPE, &subfile), 0);
    const uint16_t* r = nullptr;
    const uint16_t* g = nullptr;
    const uint16_t* bl = nullptr;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_COLORMAP, &r, &g, &bl), 1);
    EXPECT_EQ(r[0], 1234U);
    double smin = 0.0;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SMINSAMPLEVALUE, &smin), 1);
    EXPECT_DOUBLE_EQ(smin, 0.25);
  }

  // TransferFunction without BitsPerSample + BitsPerSample > 24 + wrong count
  {
    RawTiffBuilder b(false, false);
    std::vector<uint8_t> pixels(16, 0);
    uint64_t off = b.AppendBlob(pixels);
    std::vector<RawIfdEntry> e;
    e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 4));
    e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, 4));
    e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
    e.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
    e.push_back(b.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)));
    e.push_back(b.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 16));
    e.push_back(b.MakeShortArray(TIFFTAG_TRANSFERFUNCTION,
                                 std::array<uint16_t, 2>{1, 2}));
    e.push_back(b.MakeShort(TIFFTAG_OSUBFILETYPE, OFILETYPE_REDUCEDIMAGE));
    // Codec-invalid tags under COMPRESSION_NONE are ignored cleanly:
    e.push_back(b.MakeShort(TIFFTAG_PREDICTOR, 2));
    b.AddIfd(std::move(e));

    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    EXPECT_GE(stream.diagnostics().warning_count(), 1U);
  }

  // BitsPerSample=32 ColorMap ignored (>24) + wrong TransferFunction count
  {
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b, 2, 2, 32, 1);
    e.push_back(b.MakeShortArray(TIFFTAG_COLORMAP, std::array<uint16_t, 6>{}));
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    EXPECT_GE(stream.diagnostics().warning_count(), 1U);
  }

  // Palette missing Colormap fixups: 8-bit 3-spp -> RGB; 8-bit 1-spp ->
  // MINISBLACK; <8-bit 1-spp -> MissingRequired("Colormap") error!
  for (uint16_t bps : {8, 4}) {
    for (uint16_t spp : {3, 1}) {
      if (bps == 4 && spp == 3) continue;
      RawTiffBuilder b(false, false);
      auto e = MakeBaseImageEntries(b, 4, 4, bps, spp);
      for (auto& entry : e) {
        if (entry.tag == TIFFTAG_PHOTOMETRIC) {
          entry = b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_PALETTE);
        }
      }
      e.push_back(b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG));
      if (spp == 3) {
        // Explicit ExtraSamples=0 so RGB stays 3 channels
        e.push_back(b.MakeByte(TIFFTAG_EXTRASAMPLES, 0, TIFF_SHORT));
      }
      b.AddIfd(std::move(e));
      MemoryTiffStream stream(b.Take());
      UniqueTiff tif = stream.Open("rm");
      if (bps < 8) {
        EXPECT_EQ(tif, nullptr);
        EXPECT_GE(stream.diagnostics().error_count(), 1U);
      } else {
        ASSERT_NE(tif, nullptr);
        uint16_t photo = 0;
        EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PHOTOMETRIC, &photo), 1);
        EXPECT_EQ(photo, spp == 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK);
      }
    }
  }

  // ExtraSamples auto-fill when SamplesPerPixel > color_channels
  {
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b, 4, 4, 8, 5);
    for (auto& entry : e) {
      if (entry.tag == TIFFTAG_PHOTOMETRIC) {
        entry = b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
      }
    }
    e.push_back(b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG));
    // Give only 1 ExtraSample even though 5 - 3 = 2 extra channels exist.
    e.push_back(b.MakeShort(TIFFTAG_EXTRASAMPLES, EXTRASAMPLE_ASSOCALPHA));
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    uint16_t extra_n = 0;
    const uint16_t* extra_info = nullptr;
    EXPECT_EQ(
        TIFFGetField(tif.get(), TIFFTAG_EXTRASAMPLES, &extra_n, &extra_info),
        1);
    EXPECT_EQ(extra_n, 2U);
    EXPECT_EQ(extra_info[0], EXTRASAMPLE_ASSOCALPHA);
    EXPECT_EQ(extra_info[1], EXTRASAMPLE_UNSPECIFIED);
  }

  // PersampleShort: count < spp warning, count > spp warning, unequal fail.
  {
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b, 4, 4, 8, 3);
    for (auto& entry : e) {
      if (entry.tag == TIFFTAG_BITSPERSAMPLE) {
        entry = b.MakeShortArray(TIFFTAG_BITSPERSAMPLE,
                                 std::array<uint16_t, 2>{8, 8});  // < spp=3
      }
    }
    e.push_back(b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG));
    e.push_back(b.MakeShortArray(TIFFTAG_COMPRESSION,
                                 std::array<uint16_t, 4>{1, 1, 1, 1}));  // >3
    e.push_back(b.MakeShortArray(TIFFTAG_MINSAMPLEVALUE,
                                 std::array<uint16_t, 3>{0, 0, 0}));  // == 3
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    EXPECT_GE(stream.diagnostics().warning_count(), 2U);
  }
  {
    // Unequal BitsPerSample per sample -> TIFFReadDirEntryErrPsdif -> fails!
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b, 4, 4, 8, 3);
    for (auto& entry : e) {
      if (entry.tag == TIFFTAG_BITSPERSAMPLE) {
        entry = b.MakeShortArray(TIFFTAG_BITSPERSAMPLE,
                                 std::array<uint16_t, 3>{8, 8, 16});
      }
    }
    e.push_back(b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG));
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    EXPECT_EQ(tif, nullptr);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // OJPEG hacks: PLANARCONFIG_SEPARATE with 1 strip -> CONTIG, missing
  // Photometric -> YCbCr, RGB -> YCbCr, missing BitsPerSample -> 8, missing
  // SamplesPerPixel -> 3 (or 1 for MINISBLACK).
  {
    RawTiffBuilder b(false, false);
    std::vector<uint8_t> data(32, 0);
    uint64_t off = b.AppendBlob(data);
    std::vector<RawIfdEntry> e;
    e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 4));
    e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, 4));
    e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_OJPEG));
    e.push_back(b.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)));
    e.push_back(b.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 32));
    e.push_back(b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE));
    // Omit Photometric, BitsPerSample, SamplesPerPixel -> all auto-filled!
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    uint16_t pc = 0;
    uint16_t photo = 0;
    uint16_t bps = 0;
    uint16_t spp = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PLANARCONFIG, &pc), 1);
    EXPECT_EQ(pc, PLANARCONFIG_CONTIG);
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PHOTOMETRIC, &photo), 1);
    EXPECT_EQ(photo, PHOTOMETRIC_YCBCR);
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_BITSPERSAMPLE, &bps), 1);
    EXPECT_EQ(bps, 8U);
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp), 1);
    EXPECT_EQ(spp, 3U);
  }
  {
    // OJPEG RGB -> YCbCr and MINISBLACK missing spp -> 1
    for (uint16_t in_photo : {PHOTOMETRIC_RGB, PHOTOMETRIC_MINISBLACK}) {
      RawTiffBuilder b(false, false);
      std::vector<uint8_t> data(32, 0);
      uint64_t off = b.AppendBlob(data);
      std::vector<RawIfdEntry> e;
      e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 4));
      e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, 4));
      e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_OJPEG));
      e.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, in_photo));
      e.push_back(b.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)));
      e.push_back(b.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 32));
      b.AddIfd(std::move(e));
      MemoryTiffStream stream(b.Take());
      UniqueTiff tif = stream.Open("rm");
      ASSERT_NE(tif, nullptr);
      uint16_t spp = 0;
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp), 1);
      EXPECT_EQ(spp, in_photo == PHOTOMETRIC_RGB ? 3U : 1U);
    }
  }
}

TEST(TifDirReadTest, CustomDirectoriesExifAndGps) {
  for (bool be : {false, true}) {
    for (bool big : {false, true}) {
      RawTiffBuilder b(be, big);
      auto main_entries = MakeBaseImageEntries(b);

      // Build standalone EXIF IFD (not chained on main IFD list).
      std::vector<RawIfdEntry> exif_entries;
      exif_entries.push_back(b.MakeRational(EXIFTAG_SUBJECTDISTANCE, 15, 2));
      exif_entries.push_back(b.MakeShort(EXIFTAG_EXPOSUREPROGRAM, 3));
      // Wrong count (> expected 4) trimmed warning + wrong type ignored warning
      // + < expected ignored warning + unknown anonymous tag in custom IFD.
      exif_entries.push_back(MakeBytesEntry(b, EXIFTAG_EXIFVERSION,
                                            TIFF_UNDEFINED, 5,
                                            {'0', '2', '3', '1', 'x'}));
      exif_entries.push_back(b.MakeShort(EXIFTAG_EXPOSURETIME, 10));  // wrong
      exif_entries.push_back(
          b.MakeShort(EXIFTAG_COMPONENTSCONFIGURATION, 1));  // count < 4
      exif_entries.push_back(b.MakeShort(65400, 42));        // anon in custom
      // Add unsorted entry to check TIFFReadDirectoryCheckOrder in custom IFD.
      exif_entries.push_back(b.MakeShort(EXIFTAG_LIGHTSOURCE, 1));
      const uint64_t exif_off =
          b.AddIfd(std::move(exif_entries), std::nullopt,
                   /*sort_by_tag=*/false, /*link_from_previous=*/false);

      // Build standalone GPS IFD.
      std::vector<RawIfdEntry> gps_entries;
      gps_entries.push_back(
          MakeRationalArrayEntry(b, GPSTAG_LATITUDE,
                                 std::array<std::pair<uint32_t, uint32_t>, 3>{
                                     {{37, 1}, {25, 1}, {0, 1}}}));
      gps_entries.push_back(b.MakeRational(GPSTAG_ALTITUDE, 100, 4));
      gps_entries.push_back(b.MakeAscii(GPSTAG_LATITUDEREF, "N"));
      const uint64_t gps_off =
          b.AddIfd(std::move(gps_entries), std::nullopt,
                   /*sort_by_tag=*/true, /*link_from_previous=*/false);

      main_entries.push_back(
          big ? b.MakeLong8(TIFFTAG_EXIFIFD, exif_off)
              : b.MakeLong(TIFFTAG_EXIFIFD, static_cast<uint32_t>(exif_off)));
      main_entries.push_back(
          big ? b.MakeLong8(TIFFTAG_GPSIFD, gps_off)
              : b.MakeLong(TIFFTAG_GPSIFD, static_cast<uint32_t>(gps_off)));
      const uint64_t main_off = b.AddIfd(std::move(main_entries), std::nullopt,
                                         /*sort_by_tag=*/true,
                                         /*link_from_previous=*/false);
      b.PatchHeaderFirstIfdOffset(main_off);

      DiagnosticRecorder diag;
      UniqueTiffOptions opts = MakeUniqueTiffOptions();
      diag.Install(opts.get());
      TIFFOpenOptionsSetWarnAboutUnknownTags(opts.get(), 1);
      MemoryTiffStream stream(b.Take());
      UniqueTiff tif = stream.Open("rm", opts.get());
      ASSERT_NE(tif, nullptr);

      uint64_t got_exif = 0;
      uint64_t got_gps = 0;
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_EXIFIFD, &got_exif), 1);
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_GPSIFD, &got_gps), 1);

      EXPECT_EQ(TIFFReadEXIFDirectory(tif.get(), got_exif), 1);
      float subj_dist = 0.0f;
      EXPECT_EQ(TIFFGetField(tif.get(), EXIFTAG_SUBJECTDISTANCE, &subj_dist),
                1);
      EXPECT_FLOAT_EQ(subj_dist, 7.5f);

      EXPECT_EQ(TIFFReadGPSDirectory(tif.get(), got_gps), 1);
      double* lat = nullptr;
      double alt = 0.0;
      EXPECT_EQ(TIFFGetField(tif.get(), GPSTAG_LATITUDE, &lat), 1);
      EXPECT_DOUBLE_EQ(lat[0], 37.0);
      EXPECT_DOUBLE_EQ(lat[1], 25.0);
      EXPECT_EQ(TIFFGetField(tif.get(), GPSTAG_ALTITUDE, &alt), 1);
      EXPECT_DOUBLE_EQ(alt, 25.0);

      // Return to main IFD 0 via TIFFSetDirectory.
      EXPECT_EQ(TIFFSetDirectory(tif.get(), 0), 1);
      uint32_t w = 0;
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &w), 1);
      EXPECT_EQ(w, 4U);
    }
  }

  // SubjectDistance special values: 0 -> 0.0, 0xFFFFFFFF -> -1.0, denom==0 ->
  // -1.0, wrong count, wrong type, plus invalid custom dir offset -> 0.
  for (auto [num, den, expected] :
       std::array<std::tuple<uint32_t, uint32_t, float>, 3>{
           {{0, 1, 0.0f}, {0xFFFFFFFFU, 10, -1.0f}, {5, 0, -1.0f}}}) {
    RawTiffBuilder b(false, false);
    std::vector<RawIfdEntry> exif_entries;
    exif_entries.push_back(b.MakeRational(EXIFTAG_SUBJECTDISTANCE, num, den));
    const uint64_t exif_off =
        b.AddIfd(std::move(exif_entries), std::nullopt, true, false);
    const uint64_t main_off =
        b.AddIfd(MakeBaseImageEntries(b), std::nullopt, true, false);
    b.PatchHeaderFirstIfdOffset(main_off);
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFReadEXIFDirectory(tif.get(), exif_off), 1);
    float dist = 99.0f;
    EXPECT_EQ(TIFFGetField(tif.get(), EXIFTAG_SUBJECTDISTANCE, &dist), 1);
    EXPECT_FLOAT_EQ(dist, expected);
  }
  {
    RawTiffBuilder b(false, false);
    std::vector<RawIfdEntry> exif_entries;
    exif_entries.push_back(b.MakeShort(EXIFTAG_SUBJECTDISTANCE, 5));
    const uint64_t exif_off =
        b.AddIfd(std::move(exif_entries), std::nullopt, true, false);
    const uint64_t main_off =
        b.AddIfd(MakeBaseImageEntries(b), std::nullopt, true, false);
    b.PatchHeaderFirstIfdOffset(main_off);
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFReadEXIFDirectory(tif.get(), exif_off), 1);
    EXPECT_EQ(TIFFReadEXIFDirectory(tif.get(), 999999), 0);  // failed offset
  }
}

TEST(TifDirReadTest, StripByteCountEstimationBadCountsAndStripChopping) {
  // 1) Missing StripByteCounts on single uncompressed strip -> estimated.
  {
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b, 8, 8, 8, 1);
    std::erase_if(e, [](const RawIfdEntry& x) {
      return x.tag == TIFFTAG_STRIPBYTECOUNTS;
    });
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    const uint64_t* counts = nullptr;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_STRIPBYTECOUNTS, &counts), 1);
    EXPECT_EQ(counts[0], 64U);
  }

  // 2) Missing StripByteCounts on separate multi-strip (1 strip per plane).
  {
    RawTiffBuilder b(false, false);
    std::vector<uint8_t> data(48, 0);
    uint64_t off0 = b.AppendBlob(absl::Span<const uint8_t>(data.data(), 16));
    uint64_t off1 =
        b.AppendBlob(absl::Span<const uint8_t>(data.data() + 16, 16));
    uint64_t off2 =
        b.AppendBlob(absl::Span<const uint8_t>(data.data() + 32, 16));
    std::vector<RawIfdEntry> e;
    e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 4));
    e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, 4));
    e.push_back(b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
    e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
    e.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB));
    e.push_back(
        b.MakeLongArray(TIFFTAG_STRIPOFFSETS,
                        std::array<uint32_t, 3>{static_cast<uint32_t>(off0),
                                                static_cast<uint32_t>(off1),
                                                static_cast<uint32_t>(off2)}));
    e.push_back(b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3));
    e.push_back(b.MakeLong(TIFFTAG_ROWSPERSTRIP, 4));
    e.push_back(b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE));
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFGetStrileByteCount(tif.get(), 0), 16U);
    EXPECT_EQ(TIFFGetStrileByteCount(tif.get(), 2), 16U);
  }

  // 3) Missing StripByteCounts on contig multi-strip -> MissingRequired fails!
  {
    RawTiffBuilder b(false, false);
    std::vector<uint8_t> data(16, 0);
    uint64_t off0 = b.AppendBlob(absl::Span<const uint8_t>(data.data(), 8));
    uint64_t off1 = b.AppendBlob(absl::Span<const uint8_t>(data.data() + 8, 8));
    std::vector<RawIfdEntry> e;
    e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 4));
    e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, 4));
    e.push_back(b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
    e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
    e.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
    e.push_back(
        b.MakeLongArray(TIFFTAG_STRIPOFFSETS,
                        std::array<uint32_t, 2>{static_cast<uint32_t>(off0),
                                                static_cast<uint32_t>(off1)}));
    e.push_back(b.MakeLong(TIFFTAG_ROWSPERSTRIP, 2));
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    EXPECT_EQ(stream.Open("rm"), nullptr);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 4) Missing StripByteCounts on compressed single-strip -> space & EOF trim.
  for (bool big : {false, true}) {
    RawTiffBuilder b(false, big);
    std::vector<uint8_t> compressed_dummy(24, 0x80);
    uint64_t off = b.AppendBlob(compressed_dummy);
    std::vector<RawIfdEntry> e;
    e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 4));
    e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, 4));
    e.push_back(b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
    e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS));
    e.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
    e.push_back(
        big ? b.MakeLong8(TIFFTAG_STRIPOFFSETS, off)
            : b.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)));
    // Add an indirect ASCII tag so EstimateStripByteCounts sums indirect size.
    e.push_back(b.MakeAscii(TIFFTAG_IMAGEDESCRIPTION, "indirect_string"));
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    EXPECT_GT(TIFFGetStrileByteCount(tif.get(), 0), 0U);
  }

  // 5) Missing TileByteCounts on tiled uncompressed image -> TileSize64
  // estimate.
  {
    RawTiffBuilder b(false, false);
    std::vector<uint8_t> tile(256, 0);
    uint64_t off = b.AppendBlob(tile);
    std::vector<RawIfdEntry> e;
    e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 16));
    e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, 16));
    e.push_back(b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
    e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
    e.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
    e.push_back(b.MakeLong(TIFFTAG_TILEWIDTH, 16));
    e.push_back(b.MakeLong(TIFFTAG_TILELENGTH, 16));
    e.push_back(b.MakeLong(TIFFTAG_TILEOFFSETS, static_cast<uint32_t>(off)));
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFGetStrileByteCount(tif.get(), 0), 256U);
  }

  // 6) ByteCountLooksBad (0, > filesize, < scanline*length) + ChopUp (> 8192)
  {
    RawTiffBuilder b(false, false);
    // 64 x 256 = 16384 bytes -> chopped into two 8192-byte strips!
    std::vector<uint8_t> pixels(16384, 0x55);
    uint64_t off = b.AppendBlob(pixels);
    std::vector<RawIfdEntry> e;
    e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 64));
    e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, 256));
    e.push_back(b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
    e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
    e.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
    e.push_back(b.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)));
    e.push_back(b.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 0));  // Bad zero count
    e.push_back(b.MakeLong(TIFFTAG_ROWSPERSTRIP, 256));
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");  // STRIPCHOP enabled
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFNumberOfStrips(tif.get()), 2U);
    EXPECT_EQ(TIFFGetStrileByteCount(tif.get(), 0), 8192U);
    EXPECT_EQ(TIFFGetStrileByteCount(tif.get(), 1), 8192U);
  }

  // 7) ChopUp with single row > STRIP_SIZE_DEFAULT (rowblockbytes > 8192) and
  // YCbCr subsampling chop.
  {
    RawTiffBuilder b(false, false);
    // 10000 x 4 -> rowblockbytes = 10000 > 8192 -> 4 strips of 1 row each.
    std::vector<uint8_t> pixels(40000, 0x11);
    uint64_t off = b.AppendBlob(pixels);
    std::vector<RawIfdEntry> e;
    e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 10000));
    e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, 4));
    e.push_back(b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
    e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
    e.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
    e.push_back(b.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)));
    e.push_back(b.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 40000));
    e.push_back(b.MakeLong(TIFFTAG_ROWSPERSTRIP, 4));
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFNumberOfStrips(tif.get()), 4U);
  }

  // 8) Unequal contig uncompressed StripByteCounts when nstrips > 2 ->
  // "Wrong StripByteCounts field" -> EstimateStripByteCounts.
  {
    RawTiffBuilder b(false, false);
    std::vector<uint8_t> pixels(12, 0);
    uint64_t off = b.AppendBlob(pixels);
    std::vector<RawIfdEntry> e;
    e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 4));
    e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, 3));
    e.push_back(b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
    e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
    e.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
    e.push_back(b.MakeLong(TIFFTAG_ROWSPERSTRIP, 1));
    e.push_back(b.MakeLongArray(
        TIFFTAG_STRIPOFFSETS,
        std::array<uint32_t, 3>{static_cast<uint32_t>(off),
                                static_cast<uint32_t>(off + 4),
                                static_cast<uint32_t>(off + 8)}));
    e.push_back(
        b.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS,
                        std::array<uint32_t, 3>{4, 999, 4}));  // unequal
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFGetStrileByteCount(tif.get(), 1), 4U);
    EXPECT_GE(stream.diagnostics().warning_count(), 1U);
  }

  // 9) TryChopUpUncompressedBigTiff: > 2 GiB strips across >= 2 contiguous
  // strips -> chopped down to 512 MiB virtual strips!
  {
    RawTiffBuilder b(false, /*big_tiff=*/true);
    const uint64_t strip_bytes = 65536ULL * 32770ULL;  // > 2 GiB
    std::vector<RawIfdEntry> e;
    e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 65536));
    e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, 65540));
    e.push_back(b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
    e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
    e.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
    e.push_back(b.MakeLong(TIFFTAG_ROWSPERSTRIP, 32770));
    e.push_back(b.MakeLong8Array(
        TIFFTAG_STRIPOFFSETS,
        std::array<uint64_t, 2>{1024ULL, 1024ULL + strip_bytes}));
    e.push_back(
        b.MakeLong8Array(TIFFTAG_STRIPBYTECOUNTS,
                         std::array<uint64_t, 2>{strip_bytes, strip_bytes}));
    b.AddIfd(std::move(e));

    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");  // STRIPCHOP active
    ASSERT_NE(tif, nullptr);
    // 512 MiB / 65536 = 8192 rows per strip -> ceil(65540 / 8192) = 9 strips!
    uint32_t rps = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_ROWSPERSTRIP, &rps), 1);
    EXPECT_EQ(rps, 8192U);
    EXPECT_EQ(TIFFNumberOfStrips(tif.get()), 9U);
  }
}

TEST(TifDirReadTest, DeferredAndLazyStrileLoadingAndPartialRead) {
  // Build a multi-strip image with > 520 strips so > 4096 bytes of StripOffsets
  // triggers _TIFFPartialReadStripArray page-straddling in lazy mode ("rO").
  const uint32_t kNumStrips = 600;
  for (bool be : {false, true}) {
    for (bool big : {false, true}) {
      RawTiffBuilder b(be, big);
      std::vector<uint8_t> pixels(kNumStrips * 4, 0x33);
      const uint64_t base_off = b.AppendBlob(pixels);
      std::vector<uint32_t> offs32(kNumStrips);
      std::vector<uint32_t> cnts32(kNumStrips, 4);
      std::vector<uint64_t> offs64(kNumStrips);
      std::vector<uint64_t> cnts64(kNumStrips, 4);
      for (uint32_t i = 0; i < kNumStrips; ++i) {
        offs64[i] = base_off + i * 4;
        offs32[i] = static_cast<uint32_t>(offs64[i]);
      }

      std::vector<RawIfdEntry> e;
      e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 4));
      e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, kNumStrips));
      e.push_back(b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
      e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
      e.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
      e.push_back(b.MakeLong(TIFFTAG_ROWSPERSTRIP, 1));
      if (big) {
        e.push_back(b.MakeLong8Array(TIFFTAG_STRIPOFFSETS, offs64));
        e.push_back(b.MakeLong8Array(TIFFTAG_STRIPBYTECOUNTS, cnts64));
      } else {
        e.push_back(b.MakeLongArray(TIFFTAG_STRIPOFFSETS, offs32));
        e.push_back(b.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, cnts32));
      }
      b.AddIfd(std::move(e));

      const std::vector<uint8_t> file_bytes = b.Take();

      // Mode "rD": deferred loading until _TIFFFillStriles or GetStrileOffset
      {
        MemoryTiffStream stream(file_bytes);
        UniqueTiff tif = stream.Open("rmD");
        ASSERT_NE(tif, nullptr);
        EXPECT_EQ(tif->tif_dir.td_stripoffset_p, nullptr);
        int err = 1;
        EXPECT_EQ(TIFFGetStrileOffsetWithErr(tif.get(), 0, &err), offs64[0]);
        EXPECT_EQ(err, 0);
        EXPECT_EQ(
            TIFFGetStrileByteCountWithErr(tif.get(), kNumStrips - 1, &err), 4U);
        EXPECT_EQ(err, 0);
        // Second _TIFFFillStriles returns 1 quickly.
        EXPECT_EQ(_TIFFFillStriles(tif.get()), 1);
      }

      // Mode "rO": lazy per-page loading (4096-byte cache in _TIFFPartialRead)
      {
        MemoryTiffStream stream(file_bytes);
        UniqueTiff tif = stream.Open("rmO");
        ASSERT_NE(tif, nullptr);
        int err = 1;
        // Read first, middle (near page boundary), last, and OOB strile.
        EXPECT_EQ(TIFFGetStrileOffsetWithErr(tif.get(), 550, &err),
                  offs64[550]);
        EXPECT_EQ(err, 0);
        EXPECT_EQ(TIFFGetStrileOffsetWithErr(tif.get(), 0, &err), offs64[0]);
        EXPECT_EQ(err, 0);
        EXPECT_EQ(
            TIFFGetStrileByteCountWithErr(tif.get(), kNumStrips - 1, &err), 4U);
        EXPECT_EQ(err, 0);
        EXPECT_EQ(TIFFGetStrileOffsetWithErr(tif.get(), kNumStrips, &err), 0U);
        EXPECT_EQ(err, 1);

        // Explicit _TIFFFillStriles reload resets lazy cache cleanly.
        EXPECT_EQ(_TIFFFillStriles(tif.get()), 1);
        EXPECT_EQ(TIFFGetStrileOffset(tif.get(), 100), offs64[100]);
      }
    }
  }

  // Test SHORT, SLONG8, and invalid strile array types under lazy loading
  // ("rO"), plus inconsistent <=4 vs >4 counts between offsets & bytecounts.
  {
    RawTiffBuilder b(false, /*big_tiff=*/true);
    std::vector<RawIfdEntry> e;
    e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 4));
    e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, 6));
    e.push_back(b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
    e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
    e.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
    e.push_back(b.MakeLong(TIFFTAG_ROWSPERSTRIP, 1));
    e.push_back(
        b.MakeShortArray(TIFFTAG_STRIPOFFSETS,
                         std::array<uint16_t, 6>{80, 84, 88, 92, 96, 100}));
    e.push_back(MakeSlong8ArrayEntry(b, TIFFTAG_STRIPBYTECOUNTS,
                                     std::array<int64_t, 6>{4, 4, 4, 4, 4, 4}));
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rmO");
    ASSERT_NE(tif, nullptr);
    int err = 1;
    EXPECT_EQ(TIFFGetStrileOffsetWithErr(tif.get(), 2, &err), 88U);
    EXPECT_EQ(err, 0);
    EXPECT_EQ(TIFFGetStrileByteCountWithErr(tif.get(), 5, &err), 4U);
    EXPECT_EQ(err, 0);
  }
  {
    // Inconsistent count: offsets count=2 (<=4), bytecounts count=6 (>4).
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b, 4, 6, 8, 1);
    for (auto& entry : e) {
      if (entry.tag == TIFFTAG_ROWSPERSTRIP) {
        entry = b.MakeLong(TIFFTAG_ROWSPERSTRIP, 1);
      } else if (entry.tag == TIFFTAG_STRIPOFFSETS) {
        entry = b.MakeLongArray(TIFFTAG_STRIPOFFSETS,
                                std::array<uint32_t, 2>{10, 20});
      } else if (entry.tag == TIFFTAG_STRIPBYTECOUNTS) {
        entry = b.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS,
                                std::array<uint32_t, 6>{4, 4, 4, 4, 4, 4});
      }
    }
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rmD");
    ASSERT_NE(tif, nullptr);
    int err = 0;
    EXPECT_EQ(TIFFGetStrileOffsetWithErr(tif.get(), 0, &err), 0U);
    EXPECT_EQ(err, 1);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }
}

TEST(TifDirReadTest, DirectoryListLoopDetectionAndHelpers) {
  // Build two IFDs where dir1 points back to dir0 (IFD loop!).
  RawTiffBuilder b(false, false);
  const uint64_t off0 = b.AddIfd(MakeBaseImageEntries(b));
  b.AddIfd(MakeBaseImageEntries(b), /*explicit_next_ifd_offset=*/off0);

  MemoryTiffStream stream(b.Take());
  UniqueTiff tif = stream.Open("rm");
  ASSERT_NE(tif, nullptr);

  tdir_t dirn = 999;
  uint64_t diroff = 0;
  EXPECT_EQ(_TIFFGetDirNumberFromOffset(tif.get(), off0, &dirn), 1);
  EXPECT_EQ(dirn, 0U);
  EXPECT_EQ(_TIFFGetDirNumberFromOffset(tif.get(), 0, &dirn), 0);
  EXPECT_EQ(_TIFFGetOffsetFromDirNumber(tif.get(), 0, &diroff), 1);
  EXPECT_EQ(diroff, off0);
  EXPECT_EQ(_TIFFGetOffsetFromDirNumber(tif.get(), 99, &diroff), 0);

  // Reading dir 1 succeeds; reading "dir 2" hits loop back to dir 0 and warns.
  EXPECT_EQ(TIFFReadDirectory(tif.get()), 1);
  EXPECT_EQ(TIFFReadDirectory(tif.get()), 0);
  EXPECT_GE(stream.diagnostics().warning_count(), 1U);

  // Direct tests of _TIFFCheckDirNumberAndOffset update & removal helpers.
  EXPECT_EQ(_TIFFCheckDirNumberAndOffset(tif.get(), 0, 0), 0);
  EXPECT_EQ(_TIFFCheckDirNumberAndOffset(tif.get(), 0, off0), 1);  // unchanged
  EXPECT_EQ(_TIFFCheckDirNumberAndOffset(tif.get(), 0, off0 + 1000),
            1);  // updated offset for existing dirNumber
  EXPECT_EQ(_TIFFRemoveEntryFromDirectoryListByOffset(tif.get(), off0 + 1000),
            1);
  EXPECT_EQ(_TIFFRemoveEntryFromDirectoryListByOffset(tif.get(), 987654), 1);
  EXPECT_EQ(_TIFFGetOffsetFromDirNumber(tif.get(), 0, &diroff), 0);
}

TEST(TifDirReadTest, ReadWriteModeCalcIFDDataSizeAndDeferStrileEntries) {
  // Open in "r+" (O_RDWR) so CalcFinalIFDdatasizeReading runs (sorting offsets,
  // gap bytes between out-of-line payloads, gap byte before next IFD, and EOF
  // UINT64_MAX detection) plus TIFFDeferStrileArrayWriting zeroed strile tags.
  for (bool big : {false, true}) {
    // Case 1: EOF right at end of last contiguous out-of-line tag payload.
    RawTiffBuilder b(false, big);
    auto e = MakeBaseImageEntries(b);
    e.push_back(b.MakeAscii(TIFFTAG_ARTIST, "odd4"));       // 5B -> gap byte
    e.push_back(b.MakeAscii(TIFFTAG_SOFTWARE, "libtiff"));  // 8B after align
    e.push_back(b.MakeRational(TIFFTAG_XRESOLUTION, 72, 1));
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("r+m");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(tif->tif_dir.td_dirdatasize_read,
              std::numeric_limits<uint64_t>::max());
  }
  {
    // Case 2: Multiple IFDs with non-contiguous trailing data + gap byte before
    // next IFD offset.
    RawTiffBuilder b(false, false);
    auto e0 = MakeBaseImageEntries(b);
    e0.push_back(b.MakeAscii(TIFFTAG_ARTIST, "odd4"));  // ends on odd -> +1 pad
    b.AddIfd(std::move(e0));
    b.mutable_bytes().push_back(0);  // Extra 1-byte gap before dir1 -> +1
    b.AddIfd(MakeBaseImageEntries(b));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("r+m");
    ASSERT_NE(tif, nullptr);
    EXPECT_GT(tif->tif_dir.td_dirdatasize_read, 0U);
    EXPECT_LT(tif->tif_dir.td_dirdatasize_read,
              std::numeric_limits<uint64_t>::max());
  }
  {
    // Case 3: O_RDWR empty strile entries (as written by
    // TIFFDeferStrileArrayWriting).
    RawTiffBuilder b(false, false);
    std::vector<RawIfdEntry> e;
    e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 4));
    e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, 4));
    e.push_back(b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
    e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
    e.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
    e.push_back(b.MakeRawInline(TIFFTAG_STRIPOFFSETS, 0, 0, 0));
    e.push_back(b.MakeRawInline(TIFFTAG_STRIPBYTECOUNTS, 0, 0, 0));
    b.AddIfd(std::move(e));
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("r+m");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(stream.diagnostics().warning_count(), 0U);
  }
}

TEST(TifDirReadTest, AllocationLimitsAndFaultsInHeaderAndDirectoryReads) {
  RawTiffBuilder b(false, false);
  auto e = MakeBaseImageEntries(b);
  e.push_back(b.MakeAscii(TIFFTAG_IMAGEDESCRIPTION, "hello_oom"));
  b.AddIfd(std::move(e));
  const std::vector<uint8_t> valid_bytes = b.Take();

  // Fail origdir / dir allocation via ScopedCumulatedMemAllocRemaining.
  {
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    diag.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);
    MemoryTiffStream stream(valid_bytes);
    UniqueTiff tif = stream.Open("rhm", opts.get());
    ASSERT_NE(tif, nullptr);
    {
      ScopedCumulatedMemAllocRemaining budget(tif.get(), 1);
      EXPECT_EQ(TIFFReadDirectory(tif.get()), 0);
    }
    EXPECT_GE(diag.error_count(), 1U);
  }

  // Fail td_dirdatasize_offsets allocation using ScopedSingleMemAllocLimit
  // right around sizeof(TIFFDirEntry) * 10 vs dircount * 16 bytes.
  {
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    diag.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);
    MemoryTiffStream stream(valid_bytes);
    UniqueTiff tif = stream.Open("rhm", opts.get());
    ASSERT_NE(tif, nullptr);
    {
      // Verify read-seek fault cleanly aborts TIFFFetchDirectory.
      stream.FailSeeksAfter(0);
      EXPECT_EQ(TIFFReadDirectory(tif.get()), 0);
      stream.ResetFaults();
    }
    EXPECT_GE(diag.error_count(), 1U);
  }

  // Fail custom directory td_dirdatasize_offsets under CumulatedMemAlloc.
  {
    RawTiffBuilder bc(false, false);
    std::vector<RawIfdEntry> exif_entries;
    for (uint16_t k = 0; k < 20; ++k) {
      exif_entries.push_back(bc.MakeShort(static_cast<uint16_t>(65400 + k), k));
    }
    const uint64_t exif_off =
        bc.AddIfd(std::move(exif_entries), std::nullopt, true, false);
    const uint64_t main_off =
        bc.AddIfd(MakeBaseImageEntries(bc), std::nullopt, true, false);
    bc.PatchHeaderFirstIfdOffset(main_off);

    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    diag.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);
    MemoryTiffStream stream(bc.Take());
    UniqueTiff tif = stream.Open("rm", opts.get());
    ASSERT_NE(tif, nullptr);
    {
      ScopedCumulatedMemAllocRemaining budget(tif.get(), 1);
      EXPECT_EQ(TIFFReadEXIFDirectory(tif.get(), exif_off), 0);
    }
    EXPECT_GE(diag.error_count(), 1U);
  }
}

RawIfdEntry MakeFloatScalarEntry(const RawTiffBuilder& b, uint16_t tag,
                                 float value) {
  std::vector<uint8_t> raw(4);
  EncodeU32(std::bit_cast<uint32_t>(value), b.big_endian(), raw.data());
  return MakeBytesEntry(b, tag, TIFF_FLOAT, 1, std::move(raw));
}

RawIfdEntry MakeByteArrayEntry(const RawTiffBuilder& b, uint16_t tag,
                               absl::Span<const uint8_t> vals,
                               uint16_t type = TIFF_BYTE) {
  return MakeBytesEntry(b, tag, type, vals.size(),
                        std::vector<uint8_t>(vals.begin(), vals.end()));
}

TEST(TifDirReadTest, ScalarConversionAndRangeMatrix) {
  ScopedTagExtender extender(&RegisterCustomTestTags);

  constexpr std::array<uint16_t, 11> kScalarTags = {
      kTagUint8,  kTagSint8,  kTagUint16, kTagSint16, kTagUint32, kTagSint32,
      kTagUint64, kTagSint64, kTagFloat,  kTagDouble, kTagIfd8,
  };

  for (bool be : {false, true}) {
    for (bool big : {false, true}) {
      RawTiffBuilder b(be, big);

      // Build one IFD per source type using a small positive in-range value so
      // every valid (source_type -> target_type) path succeeds in BE/LE and
      // Classic/BigTIFF.
      for (uint16_t src : {
               TIFF_BYTE,
               TIFF_UNDEFINED,
               TIFF_SBYTE,
               TIFF_SHORT,
               TIFF_SSHORT,
               TIFF_LONG,
               TIFF_SLONG,
               TIFF_LONG8,
               TIFF_SLONG8,
               TIFF_FLOAT,
               TIFF_DOUBLE,
               TIFF_RATIONAL,
               TIFF_SRATIONAL,
               TIFF_IFD,
               TIFF_IFD8,
           }) {
        auto entries = MakeBaseImageEntries(b);
        for (uint16_t tag : kScalarTags) {
          switch (src) {
            case TIFF_BYTE:
            case TIFF_UNDEFINED:
              entries.push_back(b.MakeByte(tag, 12, src));
              break;
            case TIFF_SBYTE:
              entries.push_back(MakeSbyteEntry(b, tag, 12));
              break;
            case TIFF_SHORT:
              entries.push_back(b.MakeShort(tag, 12, TIFF_SHORT));
              break;
            case TIFF_SSHORT:
              entries.push_back(MakeSshortEntry(b, tag, 12));
              break;
            case TIFF_LONG:
              entries.push_back(b.MakeLong(tag, 12, TIFF_LONG));
              break;
            case TIFF_SLONG:
              entries.push_back(b.MakeLong(tag, 12, TIFF_SLONG));
              break;
            case TIFF_LONG8:
              entries.push_back(b.MakeLong8(tag, 12, TIFF_LONG8));
              break;
            case TIFF_SLONG8:
              entries.push_back(MakeSlong8Entry(b, tag, 12));
              break;
            case TIFF_FLOAT:
              entries.push_back(MakeFloatScalarEntry(b, tag, 12.5f));
              break;
            case TIFF_DOUBLE:
              entries.push_back(b.MakeDouble(tag, 12.5));
              break;
            case TIFF_RATIONAL:
              entries.push_back(b.MakeRational(tag, 25, 2));
              break;
            case TIFF_SRATIONAL:
              entries.push_back(b.MakeSrational(tag, -25, 2));
              break;
            case TIFF_IFD:
              entries.push_back(b.MakeLong(tag, 64, TIFF_IFD));
              break;
            case TIFF_IFD8:
              entries.push_back(b.MakeLong8(tag, 64, TIFF_IFD8));
              break;
            default:
              break;
          }
        }
        b.AddIfd(std::move(entries));
      }

      // Helper to append an IFD with custom error-inducing entries.
      auto add_bad_ifd = [&](auto make_entries) {
        auto entries = MakeBaseImageEntries(b);
        make_entries(entries);
        b.AddIfd(std::move(entries));
      };

      // Signed/unsigned range errors across BYTE, SBYTE, SHORT, SSHORT, LONG,
      // SLONG, LONG8, SLONG8 sources and targets.
      for (int64_t val : {-1000000000000LL, -40000LL, -200LL, -2LL, 200LL,
                          40000LL, 100000LL, 1000000000000LL}) {
        add_bad_ifd([&](std::vector<RawIfdEntry>& e) {
          if (val >= -128 && val <= 127) {
            e.push_back(MakeSbyteEntry(b, kTagUint8, static_cast<int8_t>(val)));
            e.push_back(
                MakeSbyteEntry(b, kTagUint16, static_cast<int8_t>(val)));
            e.push_back(
                MakeSbyteEntry(b, kTagUint32, static_cast<int8_t>(val)));
            e.push_back(
                MakeSbyteEntry(b, kTagUint64, static_cast<int8_t>(val)));
            e.push_back(b.MakeByte(kTagFloat, 1));
            e.push_back(
                MakeSbyteEntry(b, kTagDouble, static_cast<int8_t>(val)));
          }
          if (val >= 0 && val <= 255) {
            e.push_back(b.MakeByte(kTagSint8, static_cast<uint8_t>(val)));
            e.push_back(b.MakeByte(kTagUint16, static_cast<uint8_t>(val)));
            e.push_back(b.MakeByte(kTagSint16, static_cast<uint8_t>(val)));
            e.push_back(b.MakeByte(kTagUint32, static_cast<uint8_t>(val)));
            e.push_back(b.MakeByte(kTagSint32, static_cast<uint8_t>(val)));
            e.push_back(b.MakeByte(kTagUint64, static_cast<uint8_t>(val)));
            e.push_back(b.MakeByte(kTagSint64, static_cast<uint8_t>(val)));
            e.push_back(b.MakeByte(kTagDouble, static_cast<uint8_t>(val)));
          }
          if (val >= 0 && val <= 65535) {
            const uint16_t u16 = static_cast<uint16_t>(val);
            e.push_back(b.MakeShort(kTagUint8, u16));
            e.push_back(b.MakeShort(kTagSint8, u16));
            e.push_back(b.MakeShort(kTagSint16, u16));
          }
          if (val >= -32768 && val <= 32767) {
            const int16_t s16 = static_cast<int16_t>(val);
            e.push_back(MakeSshortEntry(b, kTagUint8, s16));
            e.push_back(MakeSshortEntry(b, kTagSint8, s16));
            e.push_back(MakeSshortEntry(b, kTagUint16, s16));
            e.push_back(MakeSshortEntry(b, kTagUint32, s16));
            e.push_back(MakeSshortEntry(b, kTagUint64, s16));
          }
          if (val >= 0 && val <= 0xFFFFFFFFLL) {
            const uint32_t u32 = static_cast<uint32_t>(val);
            e.push_back(b.MakeLong(kTagUint8, u32));
            e.push_back(b.MakeLong(kTagSint8, u32));
            e.push_back(b.MakeLong(kTagUint16, u32));
            e.push_back(b.MakeLong(kTagSint16, u32));
            e.push_back(b.MakeLong(kTagSint32, u32));
            e.push_back(b.MakeLong(kTagSint64, u32));
          }
          if (val >= -2147483648LL && val <= 2147483647LL) {
            const int32_t s32 = static_cast<int32_t>(val);
            e.push_back(b.MakeLong(kTagUint8, std::bit_cast<uint32_t>(s32),
                                   TIFF_SLONG));
            e.push_back(b.MakeLong(kTagSint8, std::bit_cast<uint32_t>(s32),
                                   TIFF_SLONG));
            e.push_back(b.MakeLong(kTagUint16, std::bit_cast<uint32_t>(s32),
                                   TIFF_SLONG));
            e.push_back(b.MakeLong(kTagSint16, std::bit_cast<uint32_t>(s32),
                                   TIFF_SLONG));
            e.push_back(b.MakeLong(kTagUint32, std::bit_cast<uint32_t>(s32),
                                   TIFF_SLONG));
            e.push_back(b.MakeLong(kTagUint64, std::bit_cast<uint32_t>(s32),
                                   TIFF_SLONG));
          }
          if (val >= 0) {
            const uint64_t u64 = static_cast<uint64_t>(val);
            e.push_back(b.MakeLong8(kTagUint8, u64, TIFF_LONG8));
            e.push_back(b.MakeLong8(kTagSint8, u64, TIFF_LONG8));
            e.push_back(b.MakeLong8(kTagUint16, u64, TIFF_LONG8));
            e.push_back(b.MakeLong8(kTagSint16, u64, TIFF_LONG8));
            e.push_back(b.MakeLong8(kTagUint32, u64, TIFF_LONG8));
            e.push_back(b.MakeLong8(kTagSint32, u64, TIFF_LONG8));
            e.push_back(b.MakeLong8(kTagSint64, u64, TIFF_LONG8));
          }
          e.push_back(MakeSlong8Entry(b, kTagUint8, val));
          e.push_back(MakeSlong8Entry(b, kTagSint8, val));
          e.push_back(MakeSlong8Entry(b, kTagUint16, val));
          e.push_back(MakeSlong8Entry(b, kTagSint16, val));
          e.push_back(MakeSlong8Entry(b, kTagUint32, val));
          e.push_back(MakeSlong8Entry(b, kTagSint32, val));
          e.push_back(MakeSlong8Entry(b, kTagUint64, val));
        });
      }

      // Extreme unsigned values > INT32_MAX / > INT64_MAX + count!=1 errors.
      add_bad_ifd([&](std::vector<RawIfdEntry>& e) {
        e.push_back(b.MakeLong(kTagSint32, 0x90000000U));
        e.push_back(b.MakeLong8(kTagSint64, 0x9000000000000000ULL, TIFF_LONG8));
        e.push_back(b.MakeShortArray(kTagUint8, std::array<uint16_t, 2>{1, 2}));
        e.push_back(b.MakeShortArray(kTagSint8, std::array<uint16_t, 2>{1, 2}));
        e.push_back(
            b.MakeShortArray(kTagUint16, std::array<uint16_t, 2>{1, 2}));
        e.push_back(
            b.MakeShortArray(kTagSint16, std::array<uint16_t, 2>{1, 2}));
        e.push_back(
            b.MakeShortArray(kTagUint32, std::array<uint16_t, 2>{1, 2}));
        e.push_back(
            b.MakeShortArray(kTagSint32, std::array<uint16_t, 2>{1, 2}));
        e.push_back(
            b.MakeShortArray(kTagUint64, std::array<uint16_t, 2>{1, 2}));
        e.push_back(
            b.MakeShortArray(kTagSint64, std::array<uint16_t, 2>{1, 2}));
        e.push_back(b.MakeShortArray(kTagFloat, std::array<uint16_t, 2>{1, 2}));
        e.push_back(
            b.MakeShortArray(kTagDouble, std::array<uint16_t, 2>{1, 2}));
        e.push_back(b.MakeShortArray(kTagIfd8, std::array<uint16_t, 2>{1, 2}));
        e.push_back(MakeRationalArrayEntry(
            b, kTagDouble,
            std::array<std::pair<uint32_t, uint32_t>, 1>{{{5, 0}}}));
      });

      MemoryTiffStream stream(b.Take());
      UniqueTiff tif = stream.Open("rm");
      ASSERT_NE(tif, nullptr);
      do {
        uint32_t w = 0;
        EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &w), 1);
      } while (TIFFReadDirectory(tif.get()) == 1);
    }
  }
}

TEST(TifDirReadTest, ArrayConversionAndFetchTagMatrix) {
  ScopedTagExtender extender(&RegisterCustomTestTags);

  constexpr std::array<uint16_t, 11> kC16Tags = {
      kTagC16Uint8,  kTagC16Sint8,  kTagC16Uint16, kTagC16Sint16,
      kTagC16Uint32, kTagC16Sint32, kTagC16Uint64, kTagC16Sint64,
      kTagC16Float,  kTagC16Double, kTagC16Ifd8,
  };

  for (bool be : {false, true}) {
    for (bool big : {false, true}) {
      RawTiffBuilder b(be, big);

      // 1. Valid 2-element conversions for every array target & source type.
      for (uint16_t src : {
               TIFF_BYTE,
               TIFF_SBYTE,
               TIFF_SHORT,
               TIFF_SSHORT,
               TIFF_LONG,
               TIFF_SLONG,
               TIFF_LONG8,
               TIFF_SLONG8,
               TIFF_FLOAT,
               TIFF_DOUBLE,
               TIFF_RATIONAL,
               TIFF_SRATIONAL,
               TIFF_IFD,
               TIFF_IFD8,
           }) {
        auto entries = MakeBaseImageEntries(b);
        for (uint16_t tag : kC16Tags) {
          switch (src) {
            case TIFF_BYTE:
              entries.push_back(
                  MakeByteArrayEntry(b, tag, std::array<uint8_t, 2>{10, 20}));
              break;
            case TIFF_SBYTE:
              entries.push_back(
                  MakeSbyteArrayEntry(b, tag, std::array<int8_t, 2>{10, 20}));
              break;
            case TIFF_SHORT:
              entries.push_back(
                  b.MakeShortArray(tag, std::array<uint16_t, 2>{10, 20}));
              break;
            case TIFF_SSHORT:
              entries.push_back(
                  MakeSshortArrayEntry(b, tag, std::array<int16_t, 2>{10, 20}));
              break;
            case TIFF_LONG:
              entries.push_back(
                  b.MakeLongArray(tag, std::array<uint32_t, 2>{10, 20}));
              break;
            case TIFF_SLONG:
              entries.push_back(
                  MakeSlongArrayEntry(b, tag, std::array<int32_t, 2>{10, 20}));
              break;
            case TIFF_LONG8:
              entries.push_back(
                  b.MakeLong8Array(tag, std::array<uint64_t, 2>{10, 20}));
              break;
            case TIFF_SLONG8:
              entries.push_back(
                  MakeSlong8ArrayEntry(b, tag, std::array<int64_t, 2>{10, 20}));
              break;
            case TIFF_FLOAT:
              entries.push_back(MakeFloatArrayEntry(
                  b, tag, std::array<float, 2>{1.5f, -2.5f}));
              break;
            case TIFF_DOUBLE:
              entries.push_back(MakeDoubleArrayEntry(
                  b, tag, std::array<double, 2>{1.5, -2.5}));
              break;
            case TIFF_RATIONAL:
              entries.push_back(MakeRationalArrayEntry(
                  b, tag,
                  std::array<std::pair<uint32_t, uint32_t>, 2>{
                      {{3, 2}, {7, 0}}}));
              break;
            case TIFF_SRATIONAL:
              entries.push_back(MakeSrationalArrayEntry(
                  b, tag,
                  std::array<std::pair<int32_t, int32_t>, 2>{
                      {{-3, 2}, {7, 0}}}));
              break;
            case TIFF_IFD:
              entries.push_back(MakeBytesEntry(b, tag, TIFF_IFD, 2, [&] {
                std::vector<uint8_t> raw(8);
                EncodeU32(100, be, raw.data());
                EncodeU32(200, be, raw.data() + 4);
                return raw;
              }()));
              break;
            case TIFF_IFD8:
              entries.push_back(MakeBytesEntry(b, tag, TIFF_IFD8, 2, [&] {
                std::vector<uint8_t> raw(16);
                EncodeU64(100, be, raw.data());
                EncodeU64(200, be, raw.data() + 8);
                return raw;
              }()));
              break;
            default:
              break;
          }
        }
        b.AddIfd(std::move(entries));
      }

      // 2. Negative & overflow range checks inside every *Array reader +
      // C32, C0, wrong-count > 0xFFFF for C16, and non-NUL C16_ASCII.
      for (int64_t val : {-1000000000000LL, -40000LL, -200LL, -2LL, 200LL,
                          40000LL, 100000LL, 1000000000000LL}) {
        auto entries = MakeBaseImageEntries(b);
        if (val >= -128 && val <= 127) {
          std::array<int8_t, 2> a = {static_cast<int8_t>(val), 0};
          entries.push_back(MakeSbyteArrayEntry(b, kTagC16Uint8, a));
          entries.push_back(MakeSbyteArrayEntry(b, kTagC16Uint16, a));
          entries.push_back(MakeSbyteArrayEntry(b, kTagC16Uint32, a));
          entries.push_back(MakeSbyteArrayEntry(b, kTagC16Uint64, a));
          entries.push_back(MakeSbyteArrayEntry(b, kTagC16Float, a));
          entries.push_back(MakeSbyteArrayEntry(b, kTagC16Double, a));
        }
        if (val >= 0 && val <= 255) {
          std::array<uint8_t, 2> a = {static_cast<uint8_t>(val), 0};
          entries.push_back(MakeByteArrayEntry(b, kTagC16Sint8, a));
          entries.push_back(MakeByteArrayEntry(b, kTagC16Double, a));
        }
        if (val >= 0 && val <= 65535) {
          std::array<uint16_t, 2> a = {static_cast<uint16_t>(val), 0};
          entries.push_back(b.MakeShortArray(kTagC16Uint8, a));
          entries.push_back(b.MakeShortArray(kTagC16Sint8, a));
          entries.push_back(b.MakeShortArray(kTagC16Sint16, a));
        }
        if (val >= -32768 && val <= 32767) {
          std::array<int16_t, 2> a = {static_cast<int16_t>(val), 0};
          entries.push_back(MakeSshortArrayEntry(b, kTagC16Uint8, a));
          entries.push_back(MakeSshortArrayEntry(b, kTagC16Sint8, a));
          entries.push_back(MakeSshortArrayEntry(b, kTagC16Uint16, a));
          entries.push_back(MakeSshortArrayEntry(b, kTagC16Uint32, a));
          entries.push_back(MakeSshortArrayEntry(b, kTagC16Uint64, a));
        }
        if (val >= 0 && val <= 0xFFFFFFFFLL) {
          std::array<uint32_t, 2> a = {static_cast<uint32_t>(val), 0};
          entries.push_back(b.MakeLongArray(kTagC16Uint8, a));
          entries.push_back(b.MakeLongArray(kTagC16Sint8, a));
          entries.push_back(b.MakeLongArray(kTagC16Uint16, a));
          entries.push_back(b.MakeLongArray(kTagC16Sint16, a));
          entries.push_back(b.MakeLongArray(kTagC16Sint32, a));
          entries.push_back(b.MakeLongArray(kTagC16Sint64, a));
        }
        if (val >= -2147483648LL && val <= 2147483647LL) {
          std::array<int32_t, 2> a = {static_cast<int32_t>(val), 0};
          entries.push_back(MakeSlongArrayEntry(b, kTagC16Uint8, a));
          entries.push_back(MakeSlongArrayEntry(b, kTagC16Sint8, a));
          entries.push_back(MakeSlongArrayEntry(b, kTagC16Uint16, a));
          entries.push_back(MakeSlongArrayEntry(b, kTagC16Sint16, a));
          entries.push_back(MakeSlongArrayEntry(b, kTagC16Uint32, a));
          entries.push_back(MakeSlongArrayEntry(b, kTagC16Uint64, a));
        }
        if (val >= 0) {
          std::array<uint64_t, 2> a = {static_cast<uint64_t>(val), 0};
          entries.push_back(b.MakeLong8Array(kTagC16Uint8, a));
          entries.push_back(b.MakeLong8Array(kTagC16Sint8, a));
          entries.push_back(b.MakeLong8Array(kTagC16Uint16, a));
          entries.push_back(b.MakeLong8Array(kTagC16Sint16, a));
          entries.push_back(b.MakeLong8Array(kTagC16Uint32, a));
          entries.push_back(b.MakeLong8Array(kTagC16Sint32, a));
          entries.push_back(b.MakeLong8Array(kTagC16Sint64, a));
        }
        std::array<int64_t, 2> s64a = {val, 0};
        entries.push_back(MakeSlong8ArrayEntry(b, kTagC16Uint8, s64a));
        entries.push_back(MakeSlong8ArrayEntry(b, kTagC16Sint8, s64a));
        entries.push_back(MakeSlong8ArrayEntry(b, kTagC16Uint16, s64a));
        entries.push_back(MakeSlong8ArrayEntry(b, kTagC16Sint16, s64a));
        entries.push_back(MakeSlong8ArrayEntry(b, kTagC16Uint32, s64a));
        entries.push_back(MakeSlong8ArrayEntry(b, kTagC16Sint32, s64a));
        entries.push_back(MakeSlong8ArrayEntry(b, kTagC16Uint64, s64a));
        b.AddIfd(std::move(entries));
      }

      // 3. C0 / C16 / C32 special paths + error returns in TIFFFetchNormalTag.
      {
        auto entries = MakeBaseImageEntries(b);
        entries.push_back(b.MakeLongArray(
            kTagC16Sint32, std::array<uint32_t, 2>{0x90000000U, 1}));
        entries.push_back(b.MakeLong8Array(
            kTagC16Sint64, std::array<uint64_t, 2>{0x9000000000000000ULL, 1}));
        entries.push_back(MakeDoubleArrayEntry(
            b, kTagC16Float, std::array<double, 2>{1e100, -1e100}));
        // Non-NUL C16_ASCII triggers null-termination warning & realloc.
        entries.push_back(MakeBytesEntry(b, TIFFTAG_INKNAMES, TIFF_ASCII, 3,
                                         {'a', 'b', 'c'}));
        // Count > 0xFFFF triggers ErrCount on C16 tags.
        entries.push_back(b.MakeRawInline(kTagC16Uint8, TIFF_BYTE, 0x10000, 0));
        entries.push_back(
            b.MakeRawInline(TIFFTAG_INKNAMES, TIFF_ASCII, 0x10000, 0));
        // C32 numeric + IFD8 tags.
        entries.push_back(
            MakeByteArrayEntry(b, kTagC32Uint8, std::array<uint8_t, 2>{3, 4}));
        entries.push_back(
            b.MakeShortArray(kTagC32Uint16, std::array<uint16_t, 2>{5, 6}));
        entries.push_back(MakeSlongArrayEntry(b, kTagC32Sint32,
                                              std::array<int32_t, 2>{-7, 8}));
        entries.push_back(MakeDoubleArrayEntry(
            b, kTagC32Double, std::array<double, 2>{1.25, 2.5}));
        entries.push_back(
            b.MakeLong8Array(kTagC32Ifd8, std::array<uint64_t, 2>{16, 32}));
        // C0 tags (valid & wrong count).
        entries.push_back(
            MakeByteArrayEntry(b, kTagC0Uint8, std::array<uint8_t, 2>{1, 2}));
        entries.push_back(
            b.MakeShortArray(kTagC0Uint16, std::array<uint16_t, 2>{3, 4}));
        entries.push_back(
            b.MakeLongArray(kTagC0Uint32, std::array<uint32_t, 2>{5, 6}));
        entries.push_back(MakeFloatArrayEntry(b, kTagC0Float,
                                              std::array<float, 2>{1.f, 2.f}));
        entries.push_back(b.MakeByte(TIFFTAG_ACTIVEAREA, 1));  // C0 bad count
        b.AddIfd(std::move(entries));
      }
      {
        // Bad types across C0 / C16 / C32 tags.
        auto entries = MakeBaseImageEntries(b);
        entries.push_back(b.MakeAscii(kTagC0Uint8, "xx"));
        entries.push_back(b.MakeAscii(kTagC0Sint8, "xxx"));
        entries.push_back(b.MakeAscii(kTagC0Uint16, "xx"));
        entries.push_back(b.MakeAscii(kTagC0Sint16, "xx"));
        entries.push_back(b.MakeAscii(kTagC0Uint32, "xx"));
        entries.push_back(b.MakeAscii(kTagC0Sint32, "xx"));
        entries.push_back(b.MakeAscii(kTagC0Uint64, "xx"));
        entries.push_back(b.MakeAscii(kTagC0Sint64, "xx"));
        entries.push_back(b.MakeAscii(kTagC0Float, "xx"));
        entries.push_back(b.MakeAscii(kTagC0Double, "xx"));
        entries.push_back(b.MakeAscii(kTagC0Ifd8, "xx"));
        entries.push_back(b.MakeAscii(kTagC16Uint8, "x"));
        entries.push_back(b.MakeAscii(kTagC16Uint16, "x"));
        entries.push_back(b.MakeAscii(kTagC16Uint32, "x"));
        entries.push_back(b.MakeShort(TIFFTAG_INKNAMES, 1));
        entries.push_back(b.MakeShort(kTagC32Ascii, 1));
        entries.push_back(b.MakeAscii(kTagC32Uint8, "x"));
        entries.push_back(b.MakeAscii(kTagC32Uint16, "x"));
        entries.push_back(b.MakeAscii(kTagC32Sint32, "x"));
        entries.push_back(b.MakeAscii(kTagC32Double, "x"));
        entries.push_back(b.MakeAscii(kTagC32Ifd8, "x"));
        b.AddIfd(std::move(entries));
      }

      MemoryTiffStream stream(b.Take());
      UniqueTiff tif = stream.Open("rm");
      ASSERT_NE(tif, nullptr);
      do {
        uint32_t w = 0;
        EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &w), 1);
      } while (TIFFReadDirectory(tif.get()) == 1);
    }
  }
}

TEST(TifDirReadTest, FetchStripThingBigChopAndDirectoryErrors) {
  // 1. TIFFFetchStripThing: short strile count resized + > max_nstrips +
  // allocsize > 100MB > filesize + wrong type.
  {
    RawTiffBuilder b(false, false);
    std::vector<uint8_t> pixels(16, 0x11);
    uint64_t off = b.AppendBlob(pixels);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 4),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        b.MakeLongArray(
            TIFFTAG_STRIPOFFSETS,
            std::array<uint32_t, 2>{static_cast<uint32_t>(off),
                                    static_cast<uint32_t>(off + 4)}),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        b.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),  // td_nstrips = 4 > count = 2
        b.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, std::array<uint32_t, 2>{4, 4}),
    });
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    ASSERT_NE(tif, nullptr);
    const uint64_t* offs = nullptr;
    ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_STRIPOFFSETS, &offs), 1);
    EXPECT_EQ(offs[2], 0U);
    EXPECT_EQ(offs[3], 0U);
  }
  {
    // nstrips > max_nstrips (1500000 > 1000000) with short StripOffsets count.
    RawTiffBuilder b(false, false);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 1),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 1500000),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        b.MakeLong(TIFFTAG_STRIPOFFSETS, 0),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        b.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        b.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 1),
    });
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rm");
    EXPECT_EQ(tif, nullptr);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }
  {
    // StripOffsets wrong type (TIFF_ASCII -> ErrType in TIFFFetchStripThing).
    RawTiffBuilder b(false, false);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 4),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        b.MakeAscii(TIFFTAG_STRIPOFFSETS, "bad"),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        b.MakeLong(TIFFTAG_ROWSPERSTRIP, 4),
        b.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 16),
    });
    MemoryTiffStream stream(b.Take());
    EXPECT_EQ(stream.Open("rm"), nullptr);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 2. TryChopUpUncompressedBigTiff: >2GB strips chopped into <=512MB strips
  // + bailouts on non-contiguous offsets & mismatched bytecounts.
  {
    RawTiffBuilder b(false, true);
    constexpr uint64_t kHugeStrip =
        0x90000000ULL;  // > 0x7FFFFFFF (W=150M * 16)
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 150994944),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 32),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        b.MakeLong8Array(TIFFTAG_STRIPOFFSETS,
                         std::array<uint64_t, 2>{4096, 4096 + kHugeStrip}),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        b.MakeLong(TIFFTAG_ROWSPERSTRIP, 16),
        b.MakeLong8Array(TIFFTAG_STRIPBYTECOUNTS,
                         std::array<uint64_t, 2>{kHugeStrip, kHugeStrip}),
    });
    // Non-contiguous 3-strip variant (fails contiguity check at i=1).
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 150994944),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 48),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        b.MakeLong8Array(TIFFTAG_STRIPOFFSETS,
                         std::array<uint64_t, 3>{4096, 8192 + kHugeStrip,
                                                 16384 + 2 * kHugeStrip}),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        b.MakeLong(TIFFTAG_ROWSPERSTRIP, 16),
        b.MakeLong8Array(
            TIFFTAG_STRIPBYTECOUNTS,
            std::array<uint64_t, 3>{kHugeStrip, kHugeStrip, kHugeStrip}),
    });
    MemoryTiffStream stream(b.Take());
    UniqueTiff tif = stream.Open("rC");  // 'C' enables TIFF_STRIPCHOP
    ASSERT_NE(tif, nullptr);
    EXPECT_GT(TIFFNumberOfStrips(tif.get()), 2U);
    ASSERT_EQ(TIFFReadDirectory(tif.get()), 1);
    EXPECT_EQ(TIFFNumberOfStrips(tif.get()), 3U);
  }

  // 3. TIFFReadDirectory structural errors & non-recoverable OutputErr paths.
  auto open_bad = [&](std::vector<RawIfdEntry> e) {
    RawTiffBuilder b(false, false);
    b.AddIfd(std::move(e));
    MemoryTiffStream s(b.Take());
    UniqueTiff t = s.Open("rm");
    EXPECT_EQ(t, nullptr);
  };
  // Missing Compression defaults cleanly to NONE -> opens!
  {
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b);
    std::erase_if(
        e, [](const RawIfdEntry& x) { return x.tag == TIFFTAG_COMPRESSION; });
    b.AddIfd(std::move(e));
    MemoryTiffStream s(b.Take());
    UniqueTiff t = s.Open("rm");
    ASSERT_NE(t, nullptr);
  }
  // Compression differing persample values ({1, 5} with SPP=2 -> ErrPsdif),
  // bad type, and missing ImageLength / StripOffsets.
  {
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b, 4, 4, 8, 2);
    for (auto& x : e) {
      if (x.tag == TIFFTAG_COMPRESSION) {
        x = b.MakeShortArray(TIFFTAG_COMPRESSION,
                             std::array<uint16_t, 2>{1, 5});
      }
    }
    open_bad(std::move(e));
  }
  {
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b);
    for (auto& x : e) {
      if (x.tag == TIFFTAG_COMPRESSION) {
        x = b.MakeAscii(TIFFTAG_COMPRESSION, "x");
      }
    }
    open_bad(std::move(e));
  }
  {
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b);
    std::erase_if(
        e, [](const RawIfdEntry& x) { return x.tag == TIFFTAG_IMAGELENGTH; });
    open_bad(std::move(e));
  }
  {
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b);
    std::erase_if(
        e, [](const RawIfdEntry& x) { return x.tag == TIFFTAG_STRIPOFFSETS; });
    open_bad(std::move(e));
  }
  {
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b, 0, 4, 8, 1);
    open_bad(std::move(e));  // Zero scanline size
  }
  {
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b);
    e.push_back(b.MakeLong(TIFFTAG_TILEWIDTH, 0));
    e.push_back(b.MakeLong(TIFFTAG_TILELENGTH, 16));
    e.push_back(b.MakeLong(TIFFTAG_TILEOFFSETS, 8));
    e.push_back(b.MakeLong(TIFFTAG_TILEBYTECOUNTS, 16));
    open_bad(std::move(e));  // Zero tile size
  }
  {
    // SamplesPerPixel=3 on MINISBLACK with only 1 ExtraSample triggers warning
    // ("Defining non-color channels as ExtraSamples").
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b, 4, 4, 8, 3);
    e.push_back(b.MakeShort(TIFFTAG_EXTRASAMPLES, EXTRASAMPLE_UNSPECIFIED));
    b.AddIfd(std::move(e));
    MemoryTiffStream s(b.Take());
    UniqueTiff t = s.Open("rm");
    ASSERT_NE(t, nullptr);
    EXPECT_GE(s.diagnostics().warning_count(), 1U);
  }
}

TEST(TifDirReadTest, MoreEstimatePartialAndDirListEdgePaths) {
  // 1. TIFFReadCustomDirectory duplicate tag, CheckDirCount short count
  //    with matching type (TIFF_UNDEFINED), and anonymous SubjectDistance.
  {
    RawTiffBuilder b(false, false);
    std::vector<RawIfdEntry> exif_entries;
    exif_entries.push_back(b.MakeRational(EXIFTAG_SUBJECTDISTANCE, 10, 2));
    exif_entries.push_back(b.MakeShort(EXIFTAG_LIGHTSOURCE, 1));
    exif_entries.push_back(b.MakeShort(EXIFTAG_LIGHTSOURCE, 2));  // duplicate
    exif_entries.push_back(MakeBytesEntry(b, EXIFTAG_COMPONENTSCONFIGURATION,
                                          TIFF_UNDEFINED, 1,
                                          {'1'}));  // CheckDirCount(4) > 1
    const uint64_t exif_off =
        b.AddIfd(std::move(exif_entries), std::nullopt, false, false);
    const uint64_t main_off =
        b.AddIfd(MakeBaseImageEntries(b), std::nullopt, true, false);
    b.PatchHeaderFirstIfdOffset(main_off);
    MemoryTiffStream s(b.Take());
    UniqueTiff tif = s.Open("rm");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFReadEXIFDirectory(tif.get(), exif_off), 1);
    EXPECT_EQ(TIFFReadGPSDirectory(tif.get(), exif_off),
              1);  // SubjectDist anon
    EXPECT_GE(s.diagnostics().warning_count(), 1U);
  }

  // 2. EstimateStripByteCounts for compressed PLANARCONFIG_SEPARATE (with last
  //    strip offset beyond EOF) and unknown TIFF_NOTYPE tag error.
  {
    RawTiffBuilder b(false, false);
    std::vector<uint8_t> data(16, 0x77);
    const uint64_t off0 = b.AppendBlob(data);
    std::vector<RawIfdEntry> e;
    e.push_back(b.MakeLong(TIFFTAG_IMAGEWIDTH, 4));
    e.push_back(b.MakeLong(TIFFTAG_IMAGELENGTH, 4));
    e.push_back(b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
    e.push_back(b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_LZW));
    e.push_back(b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
    e.push_back(b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 2));
    e.push_back(b.MakeLong(TIFFTAG_ROWSPERSTRIP, 4));
    e.push_back(b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE));
    e.push_back(b.MakeLongArray(
        TIFFTAG_STRIPOFFSETS,
        std::array<uint32_t, 2>{static_cast<uint32_t>(off0), 999999U}));
    b.AddIfd(std::move(e));
    MemoryTiffStream s(b.Take());
    UniqueTiff tif = s.Open("rm");
    ASSERT_NE(tif, nullptr);
    const uint64_t* counts = nullptr;
    ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_STRIPBYTECOUNTS, &counts), 1);
    EXPECT_EQ(counts[1], 0U);
  }
  {
    RawTiffBuilder b(false, false);
    std::vector<RawIfdEntry> e = MakeBaseImageEntries(b);
    std::erase_if(e, [](const RawIfdEntry& x) {
      return x.tag == TIFFTAG_STRIPBYTECOUNTS;
    });
    for (auto& x : e) {
      if (x.tag == TIFFTAG_COMPRESSION) {
        x = b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_LZW);
      }
    }
    e.push_back(b.MakeRawInline(65380, 99, 1, 0));
    b.AddIfd(std::move(e));
    MemoryTiffStream s(b.Take());
    EXPECT_EQ(s.Open("rm"), nullptr);  // typewidth == 0 error in Estimate
  }

  // 3. _TIFFGetDirNumberFromOffset uncached IFD lookup triggers scan.
  {
    RawTiffBuilder b(false, false);
    b.AddIfd(MakeBaseImageEntries(b));
    const uint64_t off1 = b.AddIfd(MakeBaseImageEntries(b));
    MemoryTiffStream s(b.Take());
    UniqueTiff tif = s.Open("rm");
    ASSERT_NE(tif, nullptr);
    tdir_t found = 99;
    EXPECT_EQ(_TIFFGetDirNumberFromOffset(tif.get(), off1, &found), 1);
    EXPECT_EQ(found, 1U);
    EXPECT_EQ(_TIFFGetDirNumberFromOffset(tif.get(), 888888U, &found), 0);
  }

  // 4. _TIFFPartialReadStripArray & _TIFFFetchStrileValue error & page paths.
  {
    // StripOffsets invalid type (TIFF_ASCII, count = 6 > 4) in "rD" mode.
    RawTiffBuilder b(false, false);
    auto e = MakeBaseImageEntries(b, 4, 6, 8, 1);
    for (auto& x : e) {
      if (x.tag == TIFFTAG_ROWSPERSTRIP)
        x = b.MakeLong(TIFFTAG_ROWSPERSTRIP, 1);
      if (x.tag == TIFFTAG_STRIPOFFSETS) {
        x = b.MakeAscii(TIFFTAG_STRIPOFFSETS, "abcde");
      }
      if (x.tag == TIFFTAG_STRIPBYTECOUNTS) {
        x = b.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS,
                            std::array<uint32_t, 6>{4, 4, 4, 4, 4, 4});
      }
    }
    b.AddIfd(std::move(e));
    MemoryTiffStream s(b.Take());
    UniqueTiff tif = s.Open("rD");
    ASSERT_NE(tif, nullptr);
    int err = 0;
    EXPECT_EQ(TIFFGetStrileOffsetWithErr(tif.get(), 0, &err), 0U);
  }
  {
    // Page-boundary crossing + truncated tail on StripOffsets in "rD" mode.
    RawTiffBuilder b(false, false);
    b.mutable_bytes().resize(4090, 0);
    std::array<uint32_t, 6> offs = {8, 12, 16, 20, 24, 28};
    std::array<uint32_t, 6> cnts = {4, 4, 4, 4, 4, 4};
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 6),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        b.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        b.MakeLongArray(TIFFTAG_STRIPOFFSETS, offs),
        b.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, cnts),
    });
    MemoryTiffStream s_cross(b.Take());
    UniqueTiff tif_c = s_cross.Open("rD");
    ASSERT_NE(tif_c, nullptr);
    // Offset array starts right near 4090, spanning page boundary 4096.
    int err = 0;
    EXPECT_EQ(TIFFGetStrileOffsetWithErr(tif_c.get(), 2, &err), 16U);

    // Huge strile index > 1000000 triggers "File too short".
    RawTiffBuilder bh(false, false);
    bh.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 1500000),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        b.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        bh.MakeRawInline(TIFFTAG_STRIPOFFSETS, TIFF_LONG, 1500000, 8),
        bh.MakeRawInline(TIFFTAG_STRIPBYTECOUNTS, TIFF_LONG, 1500000, 8),
    });
    MemoryTiffStream s_huge(bh.Take());
    UniqueTiff tif_h = s_huge.Open("rD");
    ASSERT_NE(tif_h, nullptr);
    EXPECT_EQ(TIFFGetStrileOffsetWithErr(tif_h.get(), 1200000, &err), 0U);
  }
  {
    // Out-of-line offset > INT64_MAX in BigTIFF _TIFFPartialReadStripArray.
    RawTiffBuilder bb(false, true);
    RawIfdEntry too_big = bb.MakeRawInline(TIFFTAG_STRIPOFFSETS, TIFF_LONG8, 6,
                                           0x8000000000000100ULL);
    bb.AddIfd({
        bb.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        bb.MakeLong(TIFFTAG_IMAGELENGTH, 6),
        bb.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        bb.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        bb.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        bb.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        too_big,
        bb.MakeLong8Array(TIFFTAG_STRIPBYTECOUNTS,
                          std::array<uint64_t, 6>{4, 4, 4, 4, 4, 4}),
    });
    MemoryTiffStream s_big(bb.Take());
    UniqueTiff tif_b = s_big.Open("rD");
    ASSERT_NE(tif_b, nullptr);
    int err = 0;
    EXPECT_EQ(TIFFGetStrileOffsetWithErr(tif_b.get(), 0, &err), 0U);
  }

  // 5. CalcFinalIFDdatasizeReading: manual layout with out-of-line data right
  //    at IFDendoffset + 1, equal offsets, descending offsets, and non-contig.
  {
    // Header(8) + dircount(2) + 11*12(132) + nextdir(4) = 146 (IFDendoffset).
    std::vector<uint8_t> raw(220, 0);
    raw[0] = 'I';
    raw[1] = 'I';
    EncodeU16(42, false, raw.data() + 2);
    EncodeU32(8, false, raw.data() + 4);   // first IFD at 8
    EncodeU16(11, false, raw.data() + 8);  // 11 entries -> end at 146
    auto write_e = [&](size_t idx, uint16_t tag, uint16_t type, uint32_t count,
                       uint32_t val_or_off) {
      size_t p = 10 + idx * 12;
      EncodeU16(tag, false, raw.data() + p);
      EncodeU16(type, false, raw.data() + p + 2);
      EncodeU32(count, false, raw.data() + p + 4);
      EncodeU32(val_or_off, false, raw.data() + p + 8);
    };
    write_e(0, TIFFTAG_IMAGEWIDTH, TIFF_LONG, 1, 4);
    write_e(1, TIFFTAG_IMAGELENGTH, TIFF_LONG, 1, 4);
    write_e(2, TIFFTAG_BITSPERSAMPLE, TIFF_SHORT, 1, 8);
    write_e(3, TIFFTAG_COMPRESSION, TIFF_SHORT, 1, COMPRESSION_NONE);
    write_e(4, TIFFTAG_PHOTOMETRIC, TIFF_SHORT, 1, PHOTOMETRIC_MINISBLACK);
    write_e(5, TIFFTAG_STRIPOFFSETS, TIFF_LONG, 1, 16);
    write_e(6, TIFFTAG_STRIPBYTECOUNTS, TIFF_LONG, 1, 16);
    std::memcpy(raw.data() + 147, "hello", 6);  // 147 = IFDendoffset(146) + 1
    std::memcpy(raw.data() + 153, "world", 6);  // contiguous after 147..152
    std::memcpy(raw.data() + 180, "nonct", 6);  // non-contiguous after 153..158
    write_e(7, TIFFTAG_IMAGEDESCRIPTION, TIFF_ASCII, 6, 153);
    write_e(8, TIFFTAG_SOFTWARE, TIFF_ASCII, 6, 153);    // equal offset
    write_e(9, TIFFTAG_ARTIST, TIFF_ASCII, 6, 180);      // disjoint after 153+6
    write_e(10, TIFFTAG_COPYRIGHT, TIFF_ASCII, 6, 147);  // unsorted vs 153/180
    MemoryTiffStream s(std::move(raw));
    UniqueTiff tif = s.Open("r+m");
    ASSERT_NE(tif, nullptr);
  }
}

}  // namespace
}  // namespace libtiff
