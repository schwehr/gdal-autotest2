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

// Unit tests for libtiff directory printing (libtiff/tif_print.c).

#include <stdio.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

// RAII owner for POSIX open_memstream FILE* captures without temp files.
class MemStreamFile {
 public:
  MemStreamFile() : fp_(::open_memstream(&buf_, &size_)) {}
  ~MemStreamFile() { CloseAndFree(); }

  MemStreamFile(const MemStreamFile&) = delete;
  MemStreamFile& operator=(const MemStreamFile&) = delete;

  FILE* get() const { return fp_; }

  std::string TakeString() {
    if (fp_ != nullptr) {
      std::fclose(fp_);
      fp_ = nullptr;
    }
    std::string out;
    if (buf_ != nullptr) {
      out.assign(buf_, size_);
      std::free(buf_);
      buf_ = nullptr;
      size_ = 0;
    }
    return out;
  }

 private:
  void CloseAndFree() noexcept {
    if (fp_ != nullptr) {
      std::fclose(fp_);
      fp_ = nullptr;
    }
    if (buf_ != nullptr) {
      std::free(buf_);
      buf_ = nullptr;
      size_ = 0;
    }
  }

  char* buf_ = nullptr;
  size_t size_ = 0;
  FILE* fp_ = nullptr;
};

std::string PrintDirectoryToString(
    TIFF* tif, long flags = TIFFPRINT_NONE) {  // NOLINT(runtime/int)
  MemStreamFile stream;
  EXPECT_NE(stream.get(), nullptr);
  if (stream.get() == nullptr) {
    return {};
  }
  TIFFPrintDirectory(tif, stream.get(), flags);
  return stream.TakeString();
}

void EncodeUint32Bytes(uint32_t v, bool big_endian, uint8_t* dst) {
  for (int i = 0; i < 4; ++i) {
    const int shift = big_endian ? (24 - 8 * i) : (8 * i);
    dst[i] = static_cast<uint8_t>((v >> shift) & 0xffU);
  }
}

void EncodeUint64Bytes(uint64_t v, bool big_endian, uint8_t* dst) {
  for (int i = 0; i < 8; ++i) {
    const int shift = big_endian ? (56 - 8 * i) : (8 * i);
    dst[i] = static_cast<uint8_t>((v >> shift) & 0xffULL);
  }
}

std::vector<uint8_t> PackDoubles(absl::Span<const double> vals,
                                 bool big_endian) {
  std::vector<uint8_t> out(vals.size() * 8);
  for (size_t i = 0; i < vals.size(); ++i) {
    EncodeUint64Bytes(std::bit_cast<uint64_t>(vals[i]), big_endian,
                      out.data() + i * 8);
  }
  return out;
}

std::vector<uint8_t> PackFloats(absl::Span<const float> vals, bool big_endian) {
  std::vector<uint8_t> out(vals.size() * 4);
  for (size_t i = 0; i < vals.size(); ++i) {
    EncodeUint32Bytes(std::bit_cast<uint32_t>(vals[i]), big_endian,
                      out.data() + i * 4);
  }
  return out;
}

std::vector<uint8_t> PackInt32s(absl::Span<const int32_t> vals,
                                bool big_endian) {
  std::vector<uint8_t> out(vals.size() * 4);
  for (size_t i = 0; i < vals.size(); ++i) {
    EncodeUint32Bytes(std::bit_cast<uint32_t>(vals[i]), big_endian,
                      out.data() + i * 4);
  }
  return out;
}

void ConfigureMinimalImage(TIFF* tif, uint32_t width = 4, uint32_t height = 4,
                           uint16_t bps = 8, uint16_t spp = 1) {
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bps), 1);
  EXPECT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, spp), 1);
  EXPECT_EQ(
      TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, uint16_t{PHOTOMETRIC_MINISBLACK}),
      1);
  EXPECT_EQ(
      TIFFSetField(tif, TIFFTAG_PLANARCONFIG, uint16_t{PLANARCONFIG_CONTIG}),
      1);
}

TEST(TifPrintTest, EmptyAndMinimalDirectory) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  // Fresh write directory has zero fields set; verify header only.
  ASSERT_EQ(TIFFCreateDirectory(tif.get()), 0);
  std::string empty_out = PrintDirectoryToString(tif.get(), TIFFPRINT_NONE);
  EXPECT_THAT(empty_out, HasSubstr("TIFF Directory at offset 0x0 (0)"));
  EXPECT_THAT(empty_out, Not(HasSubstr("Image Width:")));
  EXPECT_THAT(empty_out, Not(HasSubstr("Strips:")));

  ConfigureMinimalImage(tif.get(), 8, 6, 8, 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{3}), 1);
  std::string min_out = PrintDirectoryToString(tif.get(), TIFFPRINT_NONE);
  EXPECT_THAT(min_out, HasSubstr("Image Width: 8 Image Length: 6"));
  EXPECT_THAT(min_out, HasSubstr("Bits/Sample: 8"));
  EXPECT_THAT(min_out, HasSubstr("Samples/Pixel: 1"));
  EXPECT_THAT(min_out, HasSubstr("Rows/Strip: 3"));
  EXPECT_THAT(min_out, HasSubstr("Planar Configuration: single image plane"));
}

TEST(TifPrintTest, SubfileTypeFlags) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SUBFILETYPE, uint32_t{0}), 1);
  EXPECT_THAT(PrintDirectoryToString(tif.get()),
              HasSubstr("Subfile Type: (0 = 0x0)"));

  EXPECT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_SUBFILETYPE, uint32_t{FILETYPE_MASK}), 1);
  EXPECT_THAT(PrintDirectoryToString(tif.get()),
              HasSubstr("Subfile Type: transparency mask (4 = 0x4)"));

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SUBFILETYPE,
                         uint32_t{FILETYPE_REDUCEDIMAGE | FILETYPE_PAGE |
                                  FILETYPE_MASK}),
            1);
  EXPECT_THAT(PrintDirectoryToString(tif.get()),
              HasSubstr("Subfile Type: reduced-resolution image/multi-page "
                        "document/transparency mask (7 = 0x7)"));
}

TEST(TifPrintTest, DimensionsResolutionAndPosition) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{32}), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{48}), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEDEPTH, uint32_t{4}), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, uint32_t{16}), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, uint32_t{16}), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEDEPTH, uint32_t{2}), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_XRESOLUTION, 150.0), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_YRESOLUTION, 300.0), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_XPOSITION, 1.5), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_YPOSITION, 2.25), 1);

  std::string no_unit = PrintDirectoryToString(tif.get());
  EXPECT_THAT(no_unit,
              HasSubstr("Image Width: 32 Image Length: 48 Image Depth: 4"));
  EXPECT_THAT(no_unit,
              HasSubstr("Tile Width: 16 Tile Length: 16 Tile Depth: 2"));
  EXPECT_THAT(no_unit, HasSubstr("Resolution: 150, 300\n"));
  EXPECT_THAT(no_unit, HasSubstr("Position: 1.5, 2.25"));

  struct UnitCase {
    uint16_t unit;
    std::string_view expected;
  };
  constexpr std::array<UnitCase, 3> kUnits = {{
      {RESUNIT_NONE, "Resolution: 150, 300 (unitless)"},
      {RESUNIT_INCH, "Resolution: 150, 300 pixels/inch"},
      {RESUNIT_CENTIMETER, "Resolution: 150, 300 pixels/cm"},
  }};
  for (const auto& c : kUnits) {
    SCOPED_TRACE(c.expected);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_RESOLUTIONUNIT, c.unit), 1);
    EXPECT_THAT(PrintDirectoryToString(tif.get()), HasSubstr(c.expected));
  }
}

TEST(TifPrintTest, SampleFormatCompressionAndPhotometric) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  struct SampleFormatCase {
    uint16_t value;
    std::string_view expected;
  };
  constexpr std::array<SampleFormatCase, 6> kFormats = {{
      {SAMPLEFORMAT_VOID, "Sample Format: void"},
      {SAMPLEFORMAT_INT, "Sample Format: signed integer"},
      {SAMPLEFORMAT_UINT, "Sample Format: unsigned integer"},
      {SAMPLEFORMAT_IEEEFP, "Sample Format: IEEE floating point"},
      {SAMPLEFORMAT_COMPLEXINT, "Sample Format: complex signed integer"},
      {SAMPLEFORMAT_COMPLEXIEEEFP,
       "Sample Format: complex IEEE floating point"},
  }};
  for (const auto& c : kFormats) {
    SCOPED_TRACE(c.expected);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLEFORMAT, c.value), 1);
    EXPECT_THAT(PrintDirectoryToString(tif.get()), HasSubstr(c.expected));
  }

  EXPECT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, uint16_t{COMPRESSION_NONE}),
      1);
  EXPECT_THAT(PrintDirectoryToString(tif.get()),
              HasSubstr("Compression Scheme: None"));
  // Unknown compression scheme triggers numeric fallback in print.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, uint16_t{12345}), 1);
  EXPECT_THAT(PrintDirectoryToString(tif.get()),
              HasSubstr("Compression Scheme: 12345 (0x3039)"));
  EXPECT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, uint16_t{COMPRESSION_NONE}),
      1);

  struct PhotometricCase {
    uint16_t value;
    std::string_view expected;
  };
  constexpr std::array<PhotometricCase, 14> kPhotometrics = {{
      {PHOTOMETRIC_MINISWHITE, "Photometric Interpretation: min-is-white"},
      {PHOTOMETRIC_MINISBLACK, "Photometric Interpretation: min-is-black"},
      {PHOTOMETRIC_RGB, "Photometric Interpretation: RGB color"},
      {PHOTOMETRIC_PALETTE,
       "Photometric Interpretation: palette color (RGB from colormap)"},
      {PHOTOMETRIC_MASK, "Photometric Interpretation: transparency mask"},
      {PHOTOMETRIC_SEPARATED, "Photometric Interpretation: separated"},
      {PHOTOMETRIC_YCBCR, "Photometric Interpretation: YCbCr"},
      {7, "Photometric Interpretation: 7 (0x7)"},
      {PHOTOMETRIC_CIELAB, "Photometric Interpretation: CIE L*a*b*"},
      {PHOTOMETRIC_ICCLAB, "Photometric Interpretation: ICC L*a*b*"},
      {PHOTOMETRIC_ITULAB, "Photometric Interpretation: ITU L*a*b*"},
      {PHOTOMETRIC_LOGL, "Photometric Interpretation: CIE Log2(L)"},
      {PHOTOMETRIC_LOGLUV, "Photometric Interpretation: CIE Log2(L) (u',v')"},
      {PHOTOMETRIC_CFA, "Photometric Interpretation: 32803 (0x8023)"},
  }};
  for (const auto& c : kPhotometrics) {
    SCOPED_TRACE(c.expected);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, c.value), 1);
    EXPECT_THAT(PrintDirectoryToString(tif.get()), HasSubstr(c.expected));
  }
}

TEST(TifPrintTest, ExtraSamplesInkNamesAndInkSet) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{6}), 1);
  std::array<uint16_t, 3> extra = {
      EXTRASAMPLE_UNSPECIFIED,
      EXTRASAMPLE_ASSOCALPHA,
      EXTRASAMPLE_UNASSALPHA,
  };
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_EXTRASAMPLES,
                         static_cast<uint16_t>(extra.size()), extra.data()),
            1);
  EXPECT_THAT(
      PrintDirectoryToString(tif.get()),
      HasSubstr("Extra Samples: 3<unspecified, assoc-alpha, unassoc-alpha>"));

  // InkNames with embedded NUL separators and control chars for bounded ASCII.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{4}), 1);
  static constexpr char kInkNames[] = "Cyan\0Magenta\0Yellow\t\n\0Black";
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_INKNAMES,
                         static_cast<uint16_t>(sizeof(kInkNames)), kInkNames),
            1);
  // Upstream tif_dirinfo.c registers TIFFTAG_INKSET with count=1, so
  // _TIFFPrettyPrintField's value_count==2 check falls through to InkSet: 1.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_INKSET, uint16_t{INKSET_CMYK}), 1);

  std::string out = PrintDirectoryToString(tif.get());
  EXPECT_THAT(out, HasSubstr("Ink Names: Cyan, Magenta, Yellow\\t\\n, Black"));
  EXPECT_THAT(out, HasSubstr("NumberOfInks: 4"));
  EXPECT_THAT(out, HasSubstr("InkSet: 1"));
}

TEST(TifPrintTest, ThreshholdingFillOrderYCbCrHalftoneAndOrientation) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  struct EnumCase {
    uint16_t val;
    std::string_view expected;
  };
  constexpr std::array<EnumCase, 4> kThresh = {{
      {THRESHHOLD_BILEVEL, "Thresholding: bilevel art scan"},
      {THRESHHOLD_HALFTONE, "Thresholding: halftone or dithered scan"},
      {THRESHHOLD_ERRORDIFFUSE, "Thresholding: error diffused"},
      {99, "Thresholding: 99 (0x63)"},
  }};
  for (const auto& c : kThresh) {
    SCOPED_TRACE(c.expected);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_THRESHHOLDING, c.val), 1);
    EXPECT_THAT(PrintDirectoryToString(tif.get()), HasSubstr(c.expected));
  }

  EXPECT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_FILLORDER, uint16_t{FILLORDER_MSB2LSB}),
      1);
  EXPECT_THAT(PrintDirectoryToString(tif.get()),
              HasSubstr("FillOrder: msb-to-lsb"));
  EXPECT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_FILLORDER, uint16_t{FILLORDER_LSB2MSB}),
      1);
  EXPECT_THAT(PrintDirectoryToString(tif.get()),
              HasSubstr("FillOrder: lsb-to-msb"));

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, uint16_t{2},
                         uint16_t{4}),
            1);
  EXPECT_THAT(PrintDirectoryToString(tif.get()),
              HasSubstr("YCbCr Subsampling: 2, 4"));

  constexpr std::array<EnumCase, 3> kPos = {{
      {YCBCRPOSITION_CENTERED, "YCbCr Positioning: centered"},
      {YCBCRPOSITION_COSITED, "YCbCr Positioning: cosited"},
      {77, "YCbCr Positioning: 77 (0x4d)"},
  }};
  for (const auto& c : kPos) {
    SCOPED_TRACE(c.expected);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRPOSITIONING, c.val), 1);
    EXPECT_THAT(PrintDirectoryToString(tif.get()), HasSubstr(c.expected));
  }

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_HALFTONEHINTS, uint16_t{10},
                         uint16_t{220}),
            1);
  EXPECT_THAT(PrintDirectoryToString(tif.get()),
              HasSubstr("Halftone Hints: light 10 dark 220"));

  constexpr std::array<EnumCase, 8> kOrient = {{
      {ORIENTATION_TOPLEFT, "Orientation: row 0 top, col 0 lhs"},
      {ORIENTATION_TOPRIGHT, "Orientation: row 0 top, col 0 rhs"},
      {ORIENTATION_BOTRIGHT, "Orientation: row 0 bottom, col 0 rhs"},
      {ORIENTATION_BOTLEFT, "Orientation: row 0 bottom, col 0 lhs"},
      {ORIENTATION_LEFTTOP, "Orientation: row 0 lhs, col 0 top"},
      {ORIENTATION_RIGHTTOP, "Orientation: row 0 rhs, col 0 top"},
      {ORIENTATION_RIGHTBOT, "Orientation: row 0 rhs, col 0 bottom"},
      {ORIENTATION_LEFTBOT, "Orientation: row 0 lhs, col 0 bottom"},
  }};
  for (const auto& c : kOrient) {
    SCOPED_TRACE(c.expected);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ORIENTATION, c.val), 1);
    EXPECT_THAT(PrintDirectoryToString(tif.get()), HasSubstr(c.expected));
  }
}

TEST(TifPrintTest, RowsPerStripSampleValuesPlanarAndPageNumber) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
  EXPECT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{0xffffffffU}), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_MINSAMPLEVALUE, uint16_t{1}), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_MAXSAMPLEVALUE, uint16_t{250}), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PERSAMPLE, PERSAMPLE_MERGED), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SMINSAMPLEVALUE, -2.5), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SMAXSAMPLEVALUE, 9.75), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG,
                         uint16_t{PLANARCONFIG_CONTIG}),
            1);
  EXPECT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_PAGENUMBER, uint16_t{1}, uint16_t{4}), 1);

  std::string merged = PrintDirectoryToString(tif.get());
  EXPECT_THAT(merged, HasSubstr("Rows/Strip: (infinite)"));
  EXPECT_THAT(merged, HasSubstr("Min Sample Value: 1"));
  EXPECT_THAT(merged, HasSubstr("Max Sample Value: 250"));
  EXPECT_THAT(merged, HasSubstr("SMin Sample Value: -2.5\n"));
  EXPECT_THAT(merged, HasSubstr("SMax Sample Value: 9.75\n"));
  EXPECT_THAT(merged, HasSubstr("Planar Configuration: single image plane"));
  EXPECT_THAT(merged, HasSubstr("Page Number: 1-4"));

  std::array<double, 3> smin = {-1.0, -2.0, -3.0};
  std::array<double, 3> smax = {10.0, 20.0, 30.0};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PERSAMPLE, PERSAMPLE_MULTI), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SMINSAMPLEVALUE, smin.data()), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SMAXSAMPLEVALUE, smax.data()), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG,
                         uint16_t{PLANARCONFIG_SEPARATE}),
            1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{12}), 1);

  std::string per_sample = PrintDirectoryToString(tif.get());
  EXPECT_THAT(per_sample, HasSubstr("Rows/Strip: 12"));
  EXPECT_THAT(per_sample, HasSubstr("SMin Sample Value: -1 -2 -3"));
  EXPECT_THAT(per_sample, HasSubstr("SMax Sample Value: 10 20 30"));
  EXPECT_THAT(per_sample,
              HasSubstr("Planar Configuration: separate image planes"));
}

TEST(TifPrintTest, ColorMapRefBlackWhiteTransferFunctionAndGuard) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{2}), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);

  std::array<uint16_t, 4> red = {0, 100, 200, 300};
  std::array<uint16_t, 4> green = {10, 110, 210, 310};
  std::array<uint16_t, 4> blue = {20, 120, 220, 320};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COLORMAP, red.data(), green.data(),
                         blue.data()),
            1);
  std::array<float, 6> rbw = {0.0F, 255.0F, 0.0F, 127.0F, 0.0F, 127.0F};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_REFERENCEBLACKWHITE, rbw.data()),
            1);
  std::array<uint16_t, 4> tf0 = {0, 1000, 2000, 3000};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_TRANSFERFUNCTION, tf0.data()), 1);

  // Without flags, ColorMap and TransferFunction show "(present)".
  std::string summary = PrintDirectoryToString(tif.get(), TIFFPRINT_NONE);
  EXPECT_THAT(summary, HasSubstr("Color Map: (present)"));
  EXPECT_THAT(summary, HasSubstr("Reference Black/White:"));
  EXPECT_THAT(summary, HasSubstr(" 0:     0   255"));
  EXPECT_THAT(summary, HasSubstr(" 1:     0   127"));
  EXPECT_THAT(summary, HasSubstr("Transfer Function: (present)"));

  std::string expanded =
      PrintDirectoryToString(tif.get(), TIFFPRINT_COLORMAP | TIFFPRINT_CURVES);
  EXPECT_THAT(expanded, HasSubstr("     0:     0    10    20"));
  EXPECT_THAT(expanded, HasSubstr("     3:   300   310   320"));
  EXPECT_THAT(expanded, HasSubstr("     0:     0\n"));
  EXPECT_THAT(expanded, HasSubstr("     3:  3000\n"));

  // Check BitsPerSample >= 64 safety branch, ensuring cleanup resets bps to 2
  // before TIFFClose serializes the directory.
  absl::Cleanup reset_safe = [&tif] {
    TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{2});
    TIFFUnsetField(tif.get(), TIFFTAG_COLORMAP);
    TIFFUnsetField(tif.get(), TIFFTAG_TRANSFERFUNCTION);
  };
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{64}), 1);
  std::string too_large =
      PrintDirectoryToString(tif.get(), TIFFPRINT_COLORMAP | TIFFPRINT_CURVES);
  EXPECT_THAT(too_large,
              HasSubstr("(BitsPerSample too large to print safely)"));
  std::move(reset_safe).Invoke();

  // 3-channel TransferFunction with TIFFPRINT_CURVES.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
  std::array<uint16_t, 4> tf1 = {1, 1001, 2001, 3001};
  std::array<uint16_t, 4> tf2 = {2, 1002, 2002, 3002};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_TRANSFERFUNCTION, tf0.data(),
                         tf1.data(), tf2.data()),
            1);
  std::string curves3 = PrintDirectoryToString(tif.get(), TIFFPRINT_CURVES);
  EXPECT_THAT(curves3, HasSubstr("     0:     0     1     2"));
  EXPECT_THAT(curves3, HasSubstr("     3:  3000  3001  3002"));
}

TEST(TifPrintTest, SubIfdAndStripsAndTilesPrinting) {
  MemoryTiffStream strip_stream;
  {
    UniqueTiff w = strip_stream.Open("w");
    ASSERT_NE(w, nullptr);
    ConfigureMinimalImage(w.get(), 4, 8, 8, 1);
    EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{2}), 1);
    std::array<uint64_t, 2> sub_offsets = {0, 0};
    EXPECT_EQ(
        TIFFSetField(w.get(), TIFFTAG_SUBIFD, uint16_t{2}, sub_offsets.data()),
        1);
    std::array<uint8_t, 4> row = {1, 2, 3, 4};
    for (uint32_t r = 0; r < 8; ++r) {
      std::array<uint8_t, 4> buf = row;
      EXPECT_EQ(TIFFWriteScanline(w.get(), buf.data(), r, 0), 1);
    }
    EXPECT_EQ(TIFFWriteDirectory(w.get()), 1);

    for (int sub = 0; sub < 2; ++sub) {
      SCOPED_TRACE(sub);
      ConfigureMinimalImage(w.get(), 2, 2, 8, 1);
      std::array<uint8_t, 2> sub_row = {7, 8};
      for (uint32_t r = 0; r < 2; ++r) {
        std::array<uint8_t, 2> buf = sub_row;
        EXPECT_EQ(TIFFWriteScanline(w.get(), buf.data(), r, 0), 1);
      }
      EXPECT_EQ(TIFFWriteDirectory(w.get()), 1);
    }
  }
  {
    UniqueTiff r = strip_stream.Open("r");
    ASSERT_NE(r, nullptr);
    std::string without_strips =
        PrintDirectoryToString(r.get(), TIFFPRINT_NONE);
    EXPECT_THAT(without_strips, HasSubstr("SubIFD Offsets:"));
    EXPECT_THAT(without_strips, Not(HasSubstr("Strips:")));

    std::string with_strips = PrintDirectoryToString(r.get(), TIFFPRINT_STRIPS);
    EXPECT_THAT(with_strips, HasSubstr("SubIFD Offsets:"));
    EXPECT_THAT(with_strips, HasSubstr("  4 Strips:"));
    EXPECT_THAT(with_strips, HasSubstr("      0: ["));
    EXPECT_THAT(with_strips, HasSubstr("      3: ["));
    r.reset();
    EXPECT_EQ(strip_stream.diagnostics().error_count(), 0U);
  }

  MemoryTiffStream tile_stream;
  {
    UniqueTiff w = tile_stream.Open("w");
    ASSERT_NE(w, nullptr);
    ConfigureMinimalImage(w.get(), 32, 32, 8, 1);
    EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_TILEWIDTH, uint32_t{16}), 1);
    EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_TILELENGTH, uint32_t{16}), 1);
    std::vector<uint8_t> tile(256, 0x2A);
    for (uint32_t y = 0; y < 32; y += 16) {
      for (uint32_t x = 0; x < 32; x += 16) {
        std::vector<uint8_t> buf = tile;
        EXPECT_EQ(
            TIFFWriteEncodedTile(w.get(), TIFFComputeTile(w.get(), x, y, 0, 0),
                                 buf.data(), buf.size()),
            static_cast<tmsize_t>(buf.size()));
      }
    }
    EXPECT_EQ(TIFFWriteDirectory(w.get()), 1);
  }
  {
    UniqueTiff r = tile_stream.Open("r");
    ASSERT_NE(r, nullptr);
    std::string tiled_out = PrintDirectoryToString(r.get(), TIFFPRINT_STRIPS);
    EXPECT_THAT(tiled_out, HasSubstr("  4 Tiles:"));
    EXPECT_THAT(tiled_out, HasSubstr("      0: ["));
    EXPECT_THAT(tiled_out, HasSubstr("      3: ["));
    r.reset();
    EXPECT_EQ(tile_stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifPrintTest, PrettyPrintedAndStandardCustomTags) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ConfigureMinimalImage(tif.get(), 4, 4, 8, 1);

  EXPECT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_DOTRANGE, uint16_t{8}, uint16_t{248}), 1);
  std::array<float, 2> wp = {0.3127F, 0.3290F};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_WHITEPOINT, wp.data()), 1);

  static constexpr std::string_view kXmp = "<xmp:test/>";
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_XMLPACKET,
                         static_cast<uint32_t>(kXmp.size()), kXmp.data()),
            1);
  std::array<uint8_t, 5> iptc = {1, 2, 3, 4, 5};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_RICHTIFFIPTC,
                         static_cast<uint32_t>(iptc.size()), iptc.data()),
            1);
  std::array<uint8_t, 7> ps = {10, 20, 30, 40, 50, 60, 70};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOSHOP,
                         static_cast<uint32_t>(ps.size()), ps.data()),
            1);
  std::array<uint8_t, 9> icc = {9, 8, 7, 6, 5, 4, 3, 2, 1};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ICCPROFILE,
                         static_cast<uint32_t>(icc.size()), icc.data()),
            1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_STONITS, 150.0), 1);

  // Additional standard custom tags hitting scalar/array rational, float,
  // short, long, byte, and ASCII branches in _TIFFPrintField.
  std::array<float, 6> prim = {0.64F, 0.33F, 0.30F, 0.60F, 0.15F, 0.06F};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PRIMARYCHROMATICITIES, prim.data()),
            1);
  std::array<float, 3> cm1 = {-0.5F, 0.25F, 1.5F};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COLORMATRIX1,
                         static_cast<uint16_t>(cm1.size()), cm1.data()),
            1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_BASELINEEXPOSURE, -1.25), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PIXAR_FOVCOT, 1.75), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_CELLWIDTH, uint16_t{16}), 1);
  EXPECT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_PIXAR_IMAGEFULLWIDTH, uint32_t{640}), 1);
  std::array<uint8_t, 4> dng = {1, 4, 0, 0};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_DNGVERSION, dng.data()), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ARTIST, "CleanRoomTest"), 1);

  std::string out = PrintDirectoryToString(tif.get());
  EXPECT_THAT(out, HasSubstr("Dot Range: 8-248"));
  EXPECT_THAT(out, HasSubstr("White Point: 0.3127-0.329"));
  EXPECT_THAT(out, HasSubstr("XMLPacket (XMP Metadata):\n<xmp:test/>\n"));
  EXPECT_THAT(out, HasSubstr("RichTIFFIPTC Data: <present>, 5 bytes"));
  EXPECT_THAT(out, HasSubstr("Photoshop Data: <present>, 7 bytes"));
  EXPECT_THAT(out, HasSubstr("ICC Profile: <present>, 9 bytes"));
  EXPECT_THAT(out, HasSubstr("Sample to Nits conversion factor: 1.5000e+02"));
  EXPECT_THAT(out, HasSubstr("PrimaryChromaticities: "));
  EXPECT_THAT(out, HasSubstr("ColorMatrix1: -0.500000,0.250000,1.500000"));
  EXPECT_THAT(out, HasSubstr("BaselineExposure: -1.250000"));
  EXPECT_THAT(out, HasSubstr("FieldOfViewCotangent: 1.750000"));
  EXPECT_THAT(out, HasSubstr("CellWidth: 16"));
  EXPECT_THAT(out, HasSubstr("ImageFullWidth: 640"));
  EXPECT_THAT(out, HasSubstr("DNGVersion: 1,4,0,0"));
  EXPECT_THAT(out, HasSubstr("Artist: CleanRoomTest"));
}

TEST(TifPrintTest, CustomMergedFieldTypesSppDoubleAndUnsupported) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ConfigureMinimalImage(tif.get(), 4, 4, 8, 3);

  // _TIFFSetGetType() used by TIFFMergeFieldInfo maps TIFF_SPP to
  // TIFF_SETGET_UNDEFINED (tv_size == 1), so register a non-short-overflowing
  // TIFF_SPP field via _TIFFMergeFields with explicit TIFF_SETGET_C0_UINT16.
  static const TIFFField kSppField = {
      65100,        TIFF_SPP, TIFF_SPP,
      TIFF_SHORT,   0,        TIFF_SETGET_C0_UINT16,
      FIELD_CUSTOM, 1,        0,
      "SppShorts",  nullptr,
  };
  ASSERT_EQ(_TIFFMergeFields(tif.get(), &kSppField, 1), 1);

  static const std::array<TIFFFieldInfo, 2> kCustomFields = {{
      {65101, 2, 2, TIFF_DOUBLE, FIELD_CUSTOM, 1, 0, "DoublePair"},
      {65102, 2, 2, static_cast<TIFFDataType>(99), FIELD_CUSTOM, 1, 0,
       "UnsupportedType"},
  }};
  ASSERT_EQ(TIFFMergeFieldInfo(tif.get(), kCustomFields.data(),
                               static_cast<uint32_t>(kCustomFields.size())),
            0);

  std::array<uint16_t, 3> spp_vals = {11, 22, 33};
  EXPECT_EQ(TIFFSetField(tif.get(), 65100, spp_vals.data()), 1);
  std::array<double, 2> dbl_vals = {3.25, -7.5};
  EXPECT_EQ(TIFFSetField(tif.get(), 65101, dbl_vals.data()), 1);
  std::array<uint8_t, 2> raw_vals = {1, 2};
  EXPECT_EQ(TIFFSetField(tif.get(), 65102, raw_vals.data()), 1);

  absl::Cleanup unset_bad_type = [&tif] { TIFFUnsetField(tif.get(), 65102); };

  std::string out = PrintDirectoryToString(tif.get());
  EXPECT_THAT(out, HasSubstr("SppShorts: 11,22,33"));
  EXPECT_THAT(out, HasSubstr("DoublePair: 3.250000,-7.500000"));
  EXPECT_THAT(
      out, HasSubstr("UnsupportedType: <unsupported data type in TIFFPrint>"));
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifPrintTest, AnonymousAndAllNumericTypesViaRawTiffBuilder) {
  for (bool big_endian : {false, true}) {
    SCOPED_TRACE(big_endian ? "BigEndian" : "LittleEndian");
    RawTiffBuilder builder(big_endian, /*big_tiff=*/false);
    std::array<uint8_t, 16> pixels{};
    uint64_t strip_off = builder.AppendBlob(pixels);

    // 1-byte types need count > 4 in ClassicTIFF to be stored out-of-line;
    // <= 4-byte arrays use inline storage in the IFD value/offset slot.
    std::vector<uint8_t> sbytes = {
        std::bit_cast<uint8_t>(int8_t{-5}),
        std::bit_cast<uint8_t>(int8_t{42}),
        std::bit_cast<uint8_t>(int8_t{-128}),
        std::bit_cast<uint8_t>(int8_t{0}),
        std::bit_cast<uint8_t>(int8_t{127}),
    };
    std::array<uint16_t, 2> sshorts_u16 = {
        std::bit_cast<uint16_t>(int16_t{-300}),
        std::bit_cast<uint16_t>(int16_t{400}),
    };
    std::vector<int32_t> slongs = {-100000, 200000};
    std::vector<float> floats = {1.25F, -2.5F};
    std::vector<double> doubles = {3.5, -4.25};

    std::vector<RawIfdEntry> entries = {
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 4),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                         static_cast<uint32_t>(strip_off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 4),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(pixels.size())),
        builder.MakeAscii(TIFFTAG_GDAL_METADATA, "<GDALMetadata/>"),
        builder.MakeAscii(TIFFTAG_GDAL_NODATA, "-9999"),
        builder.MakeAscii(65001, "AnonText"),
        builder.MakeOutOfLine(65002, TIFF_BYTE, 5, {10, 20, 30, 40, 50}),
        builder.MakeOutOfLine(65003, TIFF_UNDEFINED, 5,
                              {0xab, 0xcd, 0x1, 0x2, 0x3}),
        builder.MakeOutOfLine(65004, TIFF_SBYTE, 5, std::move(sbytes)),
        builder.MakeShortArray(65005, sshorts_u16, TIFF_SSHORT),
        builder.MakeOutOfLine(65006, TIFF_SLONG, 2,
                              PackInt32s(slongs, big_endian)),
        builder.MakeLong(65007, 0x1234U, TIFF_IFD),
        builder.MakeOutOfLine(65008, TIFF_FLOAT, 2,
                              PackFloats(floats, big_endian)),
        builder.MakeOutOfLine(65009, TIFF_DOUBLE, 2,
                              PackDoubles(doubles, big_endian)),
        builder.MakeByte(65010, 99),
    };
    builder.AddIfd(std::move(entries));

    MemoryTiffStream stream(builder.Take());
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);

    std::string out = PrintDirectoryToString(tif.get(), TIFFPRINT_NONE);
    EXPECT_THAT(out, HasSubstr("GDAL Metadata: <GDALMetadata/>"));
    EXPECT_THAT(out, HasSubstr("GDAL NoDataValue: -9999"));
    EXPECT_THAT(out, HasSubstr("Tag 65001: AnonText"));
    EXPECT_THAT(out, HasSubstr("Tag 65002: 10,20,30,40,50"));
    EXPECT_THAT(out, HasSubstr("Tag 65003: 0xab,0xcd,0x1,0x2,0x3"));
    EXPECT_THAT(out, HasSubstr("Tag 65004: -5,42,-128,0,127"));
    EXPECT_THAT(out, HasSubstr("Tag 65005: -300,400"));
    EXPECT_THAT(out, HasSubstr("Tag 65006: -100000,200000"));
    EXPECT_THAT(out, HasSubstr("Tag 65007: 0x1234"));
    EXPECT_THAT(out, HasSubstr("Tag 65008: 1.250000,-2.500000"));
    EXPECT_THAT(out, HasSubstr("Tag 65009: 3.500000,-4.250000"));
    EXPECT_THAT(out, HasSubstr("Tag 65010: 99"));
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifPrintTest, BigTiffLong8Slong8Ifd8AndRoundTrip) {
  for (bool big_endian : {false, true}) {
    SCOPED_TRACE(big_endian ? "BigTiffBE" : "BigTiffLE");
    RawTiffBuilder builder(big_endian, /*big_tiff=*/true);
    std::array<uint8_t, 16> pixels{};
    uint64_t strip_off = builder.AppendBlob(pixels);

    std::array<uint64_t, 2> u64s = {0x100000001ULL, 0x200000002ULL};
    std::array<uint64_t, 2> s64s = {
        std::bit_cast<uint64_t>(int64_t{-5000000000LL}),
        std::bit_cast<uint64_t>(int64_t{6000000000LL}),
    };
    std::array<uint64_t, 2> ifd8s = {0x100000008ULL, 0x200000010ULL};

    std::vector<RawIfdEntry> entries = {
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 4),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong8(TIFFTAG_STRIPOFFSETS, strip_off),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 4),
        builder.MakeLong8(TIFFTAG_STRIPBYTECOUNTS, pixels.size()),
        builder.MakeLong8Array(65020, u64s, TIFF_LONG8),
        builder.MakeLong8Array(65021, s64s, TIFF_SLONG8),
        builder.MakeLong8Array(65022, ifd8s, TIFF_IFD8),
    };
    builder.AddIfd(std::move(entries));

    MemoryTiffStream stream(builder.Take());
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFIsBigTIFF(tif.get()), 1);

    std::string out = PrintDirectoryToString(tif.get(), TIFFPRINT_STRIPS);
    EXPECT_THAT(out, HasSubstr("Tag 65020: 4294967297,8589934594"));
    EXPECT_THAT(out, HasSubstr("Tag 65021: -5000000000,6000000000"));
    EXPECT_THAT(out, HasSubstr("Tag 65022: 0x100000008,0x200000010"));
    EXPECT_THAT(out, HasSubstr("  1 Strips:"));
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }

  // Write + read round-trip in Big-Endian BigTIFF ("wb8").
  MemoryTiffStream roundtrip_stream;
  {
    UniqueTiff w = roundtrip_stream.Open("wb8");
    ASSERT_NE(w, nullptr);
    ConfigureMinimalImage(w.get(), 4, 4, 8, 1);
    EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{4}), 1);
    std::array<uint8_t, 4> row = {10, 20, 30, 40};
    for (uint32_t r = 0; r < 4; ++r) {
      std::array<uint8_t, 4> buf = row;
      EXPECT_EQ(TIFFWriteScanline(w.get(), buf.data(), r, 0), 1);
    }
    EXPECT_EQ(TIFFWriteDirectory(w.get()), 1);
  }
  {
    UniqueTiff r = roundtrip_stream.Open("r");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(TIFFIsBigTIFF(r.get()), 1);
    EXPECT_EQ(TIFFIsByteSwapped(r.get()), 1);
    EXPECT_THAT(PrintDirectoryToString(r.get()),
                HasSubstr("Image Width: 4 Image Length: 4"));
    r.reset();
    EXPECT_EQ(roundtrip_stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifPrintTest, ExifAndGpsCustomDirectories) {
  MemoryTiffStream stream;
  {
    UniqueTiff w = stream.Open("w");
    ASSERT_NE(w, nullptr);
    ConfigureMinimalImage(w.get(), 4, 4, 8, 1);
    EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{4}), 1);
    EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_EXIFIFD, uint64_t{0}), 1);
    EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_GPSIFD, uint64_t{0}), 1);
    std::array<uint8_t, 4> row = {1, 2, 3, 4};
    for (uint32_t r = 0; r < 4; ++r) {
      std::array<uint8_t, 4> buf = row;
      EXPECT_EQ(TIFFWriteScanline(w.get(), buf.data(), r, 0), 1);
    }
    ASSERT_EQ(TIFFWriteDirectory(w.get()), 1);

    uint64_t exif_off = 0;
    ASSERT_EQ(TIFFCreateEXIFDirectory(w.get()), 0);
    std::array<uint8_t, 4> exif_ver = {'0', '2', '3', '1'};
    EXPECT_EQ(TIFFSetField(w.get(), EXIFTAG_EXIFVERSION, exif_ver.data()), 1);
    EXPECT_EQ(TIFFSetField(w.get(), EXIFTAG_PIXELXDIMENSION, uint32_t{4}), 1);
    EXPECT_EQ(TIFFSetField(w.get(), EXIFTAG_EXPOSURETIME, 0.25), 1);
    ASSERT_EQ(TIFFWriteCustomDirectory(w.get(), &exif_off), 1);

    uint64_t gps_off = 0;
    ASSERT_EQ(TIFFCreateGPSDirectory(w.get()), 0);
    std::array<uint8_t, 4> gps_ver = {2, 3, 0, 0};
    std::array<double, 3> lat = {37.0, 25.0, 19.5};
    EXPECT_EQ(TIFFSetField(w.get(), GPSTAG_VERSIONID, gps_ver.data()), 1);
    EXPECT_EQ(TIFFSetField(w.get(), GPSTAG_LATITUDEREF, "N"), 1);
    // GPSTAG_LATITUDE and GPSTAG_ALTITUDE use tv_size==8 (double precision).
    EXPECT_EQ(TIFFSetField(w.get(), GPSTAG_LATITUDE, lat.data()), 1);
    EXPECT_EQ(TIFFSetField(w.get(), GPSTAG_ALTITUDE, 123.5), 1);
    ASSERT_EQ(TIFFWriteCustomDirectory(w.get(), &gps_off), 1);

    ASSERT_EQ(TIFFSetDirectory(w.get(), 0), 1);
    EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_EXIFIFD, exif_off), 1);
    EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_GPSIFD, gps_off), 1);
    ASSERT_EQ(TIFFRewriteDirectory(w.get()), 1);
  }
  {
    UniqueTiff r = stream.Open("r");
    ASSERT_NE(r, nullptr);
    uint64_t exif_off = 0;
    uint64_t gps_off = 0;
    ASSERT_EQ(TIFFGetField(r.get(), TIFFTAG_EXIFIFD, &exif_off), 1);
    ASSERT_EQ(TIFFGetField(r.get(), TIFFTAG_GPSIFD, &gps_off), 1);
    EXPECT_GT(exif_off, 0U);
    EXPECT_GT(gps_off, 0U);

    ASSERT_EQ(TIFFReadEXIFDirectory(r.get(), exif_off), 1);
    std::string exif_out = PrintDirectoryToString(r.get());
    EXPECT_THAT(exif_out, HasSubstr("ExposureTime: 0.250000"));
    EXPECT_THAT(exif_out, HasSubstr("PixelXDimension: 4"));

    ASSERT_EQ(TIFFSetDirectory(r.get(), 0), 1);
    ASSERT_EQ(TIFFReadGPSDirectory(r.get(), gps_off), 1);
    std::string gps_out = PrintDirectoryToString(r.get());
    EXPECT_THAT(gps_out, HasSubstr("LatitudeRef: N"));
    EXPECT_THAT(gps_out, HasSubstr("Latitude: 37.000000,25.000000,19.500000"));
    EXPECT_THAT(gps_out, HasSubstr("Altitude: 123.500000"));
    r.reset();
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

void CustomPrintDirHook(TIFF* tif, FILE* fd,
                        long flags) {  // NOLINT(runtime/int)
  (void)tif;
  fprintf(fd, "  CustomPrintDirFlags: 0x%lx\n", flags);
}

TEST(TifPrintTest, CodecPrintDirHooks) {
  if (TIFFIsCODECConfigured(COMPRESSION_JPEG)) {
    MemoryTiffStream jpg_stream;
    {
      UniqueTiff w = jpg_stream.Open("w");
      ASSERT_NE(w, nullptr);
      ConfigureMinimalImage(w.get(), 16, 16, 8, 1);
      EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_JPEG}),
                1);
      EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{16}), 1);
      std::array<uint8_t, 16> row{};
      for (uint32_t r = 0; r < 16; ++r) {
        std::array<uint8_t, 16> buf = row;
        EXPECT_EQ(TIFFWriteScanline(w.get(), buf.data(), r, 0), 1);
      }
      EXPECT_EQ(TIFFWriteDirectory(w.get()), 1);
    }
    UniqueTiff r = jpg_stream.Open("r");
    ASSERT_NE(r, nullptr);
    EXPECT_THAT(PrintDirectoryToString(r.get(), TIFFPRINT_NONE),
                HasSubstr("JPEG Tables: ("));
    std::string out = PrintDirectoryToString(
        r.get(), TIFFPRINT_JPEGQTABLES | TIFFPRINT_JPEGACTABLES |
                     TIFFPRINT_JPEGDCTABLES);
    EXPECT_THAT(out, HasSubstr("Compression Scheme: JPEG"));
    EXPECT_THAT(out, HasSubstr("JPEG Tables: ("));
    r.reset();
    EXPECT_EQ(jpg_stream.diagnostics().error_count(), 0U);
  }

  if (TIFFIsCODECConfigured(COMPRESSION_CCITTFAX3)) {
    MemoryTiffStream fax3_stream;
    {
      UniqueTiff w = fax3_stream.Open("w");
      ASSERT_NE(w, nullptr);
      EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}), 1);
      EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_IMAGELENGTH, uint32_t{2}), 1);
      EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}), 1);
      EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_MINISWHITE}),
                1);
      EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_CCITTFAX3}),
                1);
      EXPECT_EQ(
          TIFFSetField(w.get(), TIFFTAG_GROUP3OPTIONS,
                       uint32_t{GROUP3OPT_2DENCODING | GROUP3OPT_FILLBITS}),
          1);
      EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_CLEANFAXDATA,
                             uint16_t{CLEANFAXDATA_REGENERATED}),
                1);
      EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_BADFAXLINES, uint32_t{2}), 1);
      EXPECT_EQ(
          TIFFSetField(w.get(), TIFFTAG_CONSECUTIVEBADFAXLINES, uint32_t{1}),
          1);
      std::array<uint8_t, 2> row = {0, 0};
      for (uint32_t r = 0; r < 2; ++r) {
        std::array<uint8_t, 2> buf = row;
        EXPECT_EQ(TIFFWriteScanline(w.get(), buf.data(), r, 0), 1);
      }
      EXPECT_EQ(TIFFWriteDirectory(w.get()), 1);
    }
    UniqueTiff r = fax3_stream.Open("r");
    ASSERT_NE(r, nullptr);
    std::string fax3_out = PrintDirectoryToString(r.get());
    EXPECT_THAT(fax3_out,
                HasSubstr("Group 3 Options: 2-d encoding+EOL padding"));
    EXPECT_THAT(fax3_out, HasSubstr("Fax Data: receiver regenerated"));
    EXPECT_THAT(fax3_out, HasSubstr("Bad Fax Lines: 2"));
    EXPECT_THAT(fax3_out, HasSubstr("Consecutive Bad Fax Lines: 1"));
    r.reset();
    EXPECT_EQ(fax3_stream.diagnostics().error_count(), 0U);
  }

  if (TIFFIsCODECConfigured(COMPRESSION_CCITTFAX4)) {
    MemoryTiffStream fax4_stream;
    {
      UniqueTiff w = fax4_stream.Open("w");
      ASSERT_NE(w, nullptr);
      ConfigureMinimalImage(w.get(), 16, 2, 1, 1);
      EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_MINISWHITE}),
                1);
      EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_CCITTFAX4}),
                1);
      EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_GROUP4OPTIONS,
                             uint32_t{GROUP4OPT_UNCOMPRESSED}),
                1);
      std::array<uint8_t, 2> row = {0, 0};
      for (uint32_t r = 0; r < 2; ++r) {
        std::array<uint8_t, 2> buf = row;
        EXPECT_EQ(TIFFWriteScanline(w.get(), buf.data(), r, 0), 1);
      }
      EXPECT_EQ(TIFFWriteDirectory(w.get()), 1);
    }
    UniqueTiff r = fax4_stream.Open("r");
    ASSERT_NE(r, nullptr);
    EXPECT_THAT(PrintDirectoryToString(r.get()),
                HasSubstr("Group 4 Options: uncompressed data"));
    EXPECT_EQ(fax4_stream.diagnostics().error_count(), 0U);
  }

  if (TIFFIsCODECConfigured(COMPRESSION_LZW)) {
    MemoryTiffStream lzw_stream;
    UniqueTiff w = lzw_stream.Open("w");
    ASSERT_NE(w, nullptr);
    ConfigureMinimalImage(w.get(), 4, 4, 8, 1);
    EXPECT_EQ(
        TIFFSetField(w.get(), TIFFTAG_COMPRESSION, uint16_t{COMPRESSION_LZW}),
        1);
    EXPECT_EQ(TIFFSetField(w.get(), TIFFTAG_PREDICTOR,
                           uint16_t{PREDICTOR_HORIZONTAL}),
              1);
    EXPECT_THAT(PrintDirectoryToString(w.get()),
                HasSubstr("Predictor: horizontal differencing 2 (0x2)"));
  }

  // Custom printdir callback directly on tagmethods, restored before close.
  MemoryTiffStream hook_stream;
  UniqueTiff tif = hook_stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ConfigureMinimalImage(tif.get(), 4, 4, 8, 1);
  TIFFTagMethods* methods = TIFFAccessTagMethods(tif.get());
  ASSERT_NE(methods, nullptr);
  TIFFPrintMethod orig_printdir = methods->printdir;
  methods->printdir = &CustomPrintDirHook;
  absl::Cleanup restore_printdir = [methods, orig_printdir] {
    methods->printdir = orig_printdir;
  };
  EXPECT_THAT(PrintDirectoryToString(tif.get(), TIFFPRINT_STRIPS),
              HasSubstr("CustomPrintDirFlags: 0x1"));
}

struct VGetFailContext {
  TIFFVGetMethod orig_vgetfield = nullptr;
};

int FailSomeCustomTagsVGetField(TIFF* tif, uint32_t tag, va_list ap) {
  switch (tag) {
    case TIFFTAG_XMLPACKET:       // passcount && TIFF_VARIABLE2
    case TIFFTAG_DNGPRIVATEDATA:  // passcount && TIFF_VARIABLE
    case TIFFTAG_DNGVERSION:      // !passcount && value_count > 1
    case TIFFTAG_CELLWIDTH:       // !passcount && scalar (allocates raw_data)
      return 0;
    default:
      break;
  }
  auto* ctx = static_cast<VGetFailContext*>(
      TIFFGetClientInfo(tif, "tif_print_test_vget"));
  if (ctx == nullptr || ctx->orig_vgetfield == nullptr) {
    return 0;
  }
  return (*ctx->orig_vgetfield)(tif, tag, ap);
}

TEST(TifPrintTest, CustomTagGetFieldFailurePaths) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ConfigureMinimalImage(tif.get(), 4, 4, 8, 1);

  static constexpr std::string_view kXmp = "<xmp/>";
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_XMLPACKET,
                         static_cast<uint32_t>(kXmp.size()), kXmp.data()),
            1);
  std::array<uint8_t, 3> priv = {1, 2, 3};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_DNGPRIVATEDATA,
                         static_cast<uint16_t>(priv.size()), priv.data()),
            1);
  std::array<uint8_t, 4> dng = {1, 4, 0, 0};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_DNGVERSION, dng.data()), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_CELLWIDTH, uint16_t{16}), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ARTIST, "KeptTag"), 1);

  TIFFTagMethods* methods = TIFFAccessTagMethods(tif.get());
  ASSERT_NE(methods, nullptr);
  VGetFailContext ctx{.orig_vgetfield = methods->vgetfield};
  TIFFSetClientInfo(tif.get(), &ctx, "tif_print_test_vget");
  methods->vgetfield = &FailSomeCustomTagsVGetField;
  absl::Cleanup restore_vget = [&tif, methods, &ctx] {
    methods->vgetfield = ctx.orig_vgetfield;
    TIFFSetClientInfo(tif.get(), nullptr, "tif_print_test_vget");
  };

  std::string out = PrintDirectoryToString(tif.get());
  EXPECT_THAT(out, HasSubstr("Artist: KeptTag"));
  EXPECT_THAT(out, Not(HasSubstr("XMLPacket")));
  EXPECT_THAT(out, Not(HasSubstr("DNGPrivateData")));
  EXPECT_THAT(out, Not(HasSubstr("DNGVersion")));
  EXPECT_THAT(out, Not(HasSubstr("CellWidth")));
}

TEST(TifPrintTest, AsciiPrintHelpers) {
  MemStreamFile stream;
  ASSERT_NE(stream.get(), nullptr);

  _TIFFprintAscii(stream.get(), "PlainASCII");
  _TIFFprintAscii(stream.get(), "");
  _TIFFprintAscii(stream.get(), "\t\b\r\n\v");
  _TIFFprintAscii(stream.get(), "\x01\x1b\x7f\x80\xff");
  _TIFFprintAsciiTag(stream.get(), "Make", "Acme\nCorp");

  std::string out = stream.TakeString();
  EXPECT_EQ(out,
            "PlainASCII\\t\\b\\r\\n\\v\\001\\033\\177\\200\\377"
            "  Make: \"Acme\\nCorp\"\n");
}

TEST(TifPrintTest, CumulatedMemAllocLimitDuringPrint) {
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  DiagnosticRecorder recorder;
  recorder.Install(opts.get());
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w", opts.get());
  ASSERT_NE(tif, nullptr);
  ConfigureMinimalImage(tif.get(), 4, 4, 8, 1);

  // Note: scalar custom tags at tif_print.c:708 lack a NULL check after
  // _TIFFCheckMalloc, so OOM is only safe here without unprinted scalar custom
  // tags; verify zero-allocation printing under tightened cumulated budget.
  ScopedCumulatedMemAllocRemaining tighten(tif.get(), 0);
  std::string out = PrintDirectoryToString(tif.get(), TIFFPRINT_NONE);
  EXPECT_THAT(out, HasSubstr("Image Width: 4 Image Length: 4"));
  EXPECT_EQ(recorder.error_count(), 0U);
}

}  // namespace
}  // namespace libtiff
