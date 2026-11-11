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

// Clean-room unit and subprocess tests for third_party/tiff/tools/tiffsplit.c.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT(build/c++17)
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/str_split.h"
#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/temp_file.h"
#include "third_party/tiff/libtiff/tiff.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;

constexpr std::string_view kTiffsplitRunfilesPath =
    "third_party/tiff/tiffsplit";

std::filesystem::path TiffsplitBinary() {
  return ResolveRunfilesPath(kTiffsplitRunfilesPath);
}

TempDir MakeTestDir(std::string_view suffix = "") {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string dir_name =
      info != nullptr
          ? absl::StrCat(info->test_suite_name(), "_", info->name(), suffix)
          : absl::StrCat("tiffsplit_test", suffix);
  return TempDir(dir_name);
}

bool WriteBytesToFile(const std::filesystem::path& path,
                      absl::Span<const uint8_t> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return false;
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

UniqueTiff OpenQuietTiff(const std::filesystem::path& path, const char* mode,
                         DiagnosticRecorder* diagnostics) {
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  if (opts == nullptr) return nullptr;
  if (diagnostics != nullptr) {
    diagnostics->Install(opts.get());
  }
  return UniqueTiff(TIFFOpenExt(path.c_str(), mode, opts.get()));
}

void WriteSimpleStripPage(TIFF* tif, uint32_t width, uint32_t height,
                          uint8_t base_val) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, height), 1);

  std::vector<uint8_t> row(width);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      row[x] = static_cast<uint8_t>(base_val + y * 13 + x);
    }
    ASSERT_EQ(TIFFWriteScanline(tif, row.data(), y, 0), 1);
  }
  ASSERT_EQ(TIFFWriteDirectory(tif), 1);
}

void WriteSimpleTilePage(TIFF* tif, uint32_t width, uint32_t height,
                         uint32_t tile_w, uint32_t tile_h, uint8_t base_val) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILEWIDTH, tile_w), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILELENGTH, tile_h), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);

  const tmsize_t tile_bytes = TIFFTileSize(tif);
  ASSERT_GT(tile_bytes, 0);
  std::vector<uint8_t> tile(static_cast<size_t>(tile_bytes));
  const ttile_t num_tiles = TIFFNumberOfTiles(tif);
  for (ttile_t t = 0; t < num_tiles; ++t) {
    std::fill(tile.begin(), tile.end(), static_cast<uint8_t>(base_val + t * 7));
    ASSERT_EQ(TIFFWriteEncodedTile(tif, t, tile.data(), tile_bytes),
              tile_bytes);
  }
  ASSERT_EQ(TIFFWriteDirectory(tif), 1);
}

TEST(TiffsplitTest, NoArgsPrintsUsageAndExitsFailure) {
  const SubprocessResult res = RunSubprocess(TiffsplitBinary(), {});
  EXPECT_EQ(res.term_signal, 0);
  EXPECT_EQ(res.exit_code, 1);
  EXPECT_THAT(res.stdout_output, IsEmpty());
  EXPECT_THAT(res.stderr_output,
              HasSubstr("Split a multi-image TIFF into single-image TIFF"));
  EXPECT_THAT(res.stderr_output,
              HasSubstr("usage: tiffsplit [option] input.tif [prefix]"));
  EXPECT_THAT(res.stderr_output, HasSubstr("-M size"));
}

TEST(TiffsplitTest, TooManyArgsPrintsUsageAndExitsFailure) {
  TempDir dir = MakeTestDir();
  const std::string in_path = dir.child("in.tif").native();
  const std::string prefix = dir.child("out_").native();

  const SubprocessResult res =
      RunSubprocess(TiffsplitBinary(), {in_path, prefix, "extra_arg"});
  EXPECT_EQ(res.term_signal, 0);
  EXPECT_EQ(res.exit_code, 1);
  EXPECT_THAT(res.stdout_output, IsEmpty());
  EXPECT_THAT(res.stderr_output, HasSubstr("usage: tiffsplit"));
}

TEST(TiffsplitTest, UnknownOptionPrintsUsageToStdoutAndExitsSuccess) {
  for (const std::vector<std::string>& args :
       {std::vector<std::string>{"-h"}, std::vector<std::string>{"-M"}}) {
    SCOPED_TRACE(args.front());
    const SubprocessResult res = RunSubprocess(TiffsplitBinary(), args);
    EXPECT_EQ(res.term_signal, 0);
    EXPECT_EQ(res.exit_code, 0);
    EXPECT_THAT(res.stdout_output, HasSubstr("LIBTIFF"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("Split a multi-image TIFF into single-image TIFF"));
    EXPECT_THAT(res.stdout_output, HasSubstr("usage: tiffsplit"));
  }
}

TEST(TiffsplitTest, InvalidMemoryLimitExitsFailure) {
  TempDir dir = MakeTestDir();
  const std::string in_path = dir.child("in.tif").native();
  const std::string prefix = dir.child("out_").native();

  for (const std::string& bad_limit : {"abc", "-1", "18446744073709551616"}) {
    SCOPED_TRACE(bad_limit);
    const SubprocessResult res =
        RunSubprocess(TiffsplitBinary(), {"-M", bad_limit, in_path, prefix});
    EXPECT_EQ(res.term_signal, 0);
    EXPECT_EQ(res.exit_code, 1);
    EXPECT_THAT(res.stderr_output, HasSubstr("usage: tiffsplit"));
  }
}

TEST(TiffsplitTest, PrefixTooLongExitsFailure) {
  TempDir dir = MakeTestDir();
  const std::string in_path = dir.child("in.tif").native();
  const std::string long_prefix(8185, 'p');

  const SubprocessResult res =
      RunSubprocess(TiffsplitBinary(), {in_path, long_prefix});
  EXPECT_EQ(res.term_signal, 0);
  EXPECT_EQ(res.exit_code, 1);
  EXPECT_THAT(res.stderr_output, HasSubstr("filename too long"));
}

TEST(TiffsplitTest, MissingInputFileExitsFailureWithAndWithoutPrefix) {
  TempDir dir = MakeTestDir();
  const std::string missing = dir.child("does_not_exist.tif").native();

  const SubprocessResult res_no_prefix =
      RunSubprocess(TiffsplitBinary(), {missing});
  EXPECT_EQ(res_no_prefix.term_signal, 0);
  EXPECT_EQ(res_no_prefix.exit_code, 1);
  EXPECT_THAT(res_no_prefix.stderr_output, HasSubstr("Could not open"));
  EXPECT_THAT(res_no_prefix.stderr_output, HasSubstr("usage: tiffsplit"));

  const std::string prefix = dir.child("out_").native();
  const SubprocessResult res_with_prefix =
      RunSubprocess(TiffsplitBinary(), {missing, prefix});
  EXPECT_EQ(res_with_prefix.term_signal, 0);
  EXPECT_EQ(res_with_prefix.exit_code, 1);
  EXPECT_THAT(res_with_prefix.stderr_output, HasSubstr("Could not open"));
}

TEST(TiffsplitTest, CorruptInputFileExitsFailure) {
  TempDir dir = MakeTestDir();
  const std::filesystem::path corrupt = dir.child("corrupt.tif");
  const std::array<uint8_t, 8> garbage = {'N', 'O', 'T', '_',
                                          'T', 'I', 'F', 'F'};
  ASSERT_TRUE(WriteBytesToFile(corrupt, garbage));

  const SubprocessResult res = RunSubprocess(
      TiffsplitBinary(), {corrupt.native(), dir.child("out_").native()});
  EXPECT_EQ(res.term_signal, 0);
  EXPECT_EQ(res.exit_code, 1);
  EXPECT_THAT(res.stderr_output, HasSubstr("Could not open"));
}

TEST(TiffsplitTest, UnwritableOutputPrefixExitsFailure) {
  TempDir dir = MakeTestDir();
  const std::filesystem::path in_path = dir.child("input.tif");
  {
    DiagnosticRecorder diagnostics;
    UniqueTiff writer = OpenQuietTiff(in_path, "wl", &diagnostics);
    ASSERT_NE(writer.get(), nullptr);
    WriteSimpleStripPage(writer.get(), 4, 4, 10);
    EXPECT_EQ(diagnostics.error_count(), 0U);
  }

  const std::string bad_prefix = dir.child("nonexistent_dir/out_").native();
  const SubprocessResult res =
      RunSubprocess(TiffsplitBinary(), {in_path.native(), bad_prefix});
  EXPECT_EQ(res.term_signal, 0);
  EXPECT_EQ(res.exit_code, 1);
  EXPECT_THAT(res.stderr_output, HasSubstr("Could not open output file"));
}

TEST(TiffsplitTest, SplitsDiverseMultiPageTiffAndPreservesTagsAndPixels) {
  TempDir dir = MakeTestDir();
  const std::filesystem::path in_path = dir.child("multipage.tif");

  std::array<uint16_t, 256> red_map{};
  std::array<uint16_t, 256> green_map{};
  std::array<uint16_t, 256> blue_map{};
  for (size_t i = 0; i < 256; ++i) {
    red_map[i] = static_cast<uint16_t>(i * 257U);
    green_map[i] = static_cast<uint16_t>((255U - i) * 257U);
    blue_map[i] = static_cast<uint16_t>(((i * 3U) & 0xffU) * 257U);
  }

  {
    DiagnosticRecorder diagnostics;
    UniqueTiff tif = OpenQuietTiff(in_path, "wl", &diagnostics);
    ASSERT_NE(tif.get(), nullptr);

    // Page 1: LZW strips with horizontal predictor, ExtraSamples, Float/ASCII,
    // Fax custom tags, SubfileType, and PageNumber.
    {
      ASSERT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_SUBFILETYPE, uint32_t{FILETYPE_PAGE}),
          1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{8}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{4}),
                1);
      uint16_t extra = EXTRASAMPLE_ASSOCALPHA;
      ASSERT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_EXTRASAMPLES, uint16_t{1}, &extra),
          1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_LZW),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PREDICTOR,
                             uint16_t{PREDICTOR_HORIZONTAL}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_RGB}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{4}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_THRESHHOLDING,
                             uint16_t{THRESHHOLD_BILEVEL}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_FILLORDER,
                             uint16_t{FILLORDER_MSB2LSB}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ORIENTATION,
                             uint16_t{ORIENTATION_TOPLEFT}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_MINSAMPLEVALUE, uint16_t{2}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_MAXSAMPLEVALUE, uint16_t{250}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_XRESOLUTION, 300.0), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YRESOLUTION, 150.0), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_RESOLUTIONUNIT,
                             uint16_t{RESUNIT_INCH}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_XPOSITION, 1.25), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YPOSITION, 2.5), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLEFORMAT,
                             uint16_t{SAMPLEFORMAT_UINT}),
                1);
      ASSERT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_PAGENUMBER, uint16_t{0}, uint16_t{8}),
          1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ARTIST, "Test Artist"), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEDESCRIPTION,
                             "LZW RGBA strip page"),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_MAKE, "TestMake"), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_MODEL, "TestModel"), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SOFTWARE, "TestSoft"), 1);
      ASSERT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_DATETIME, "2026:11:10 01:30:00"), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_HOSTCOMPUTER, "TestHost"), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PAGENAME, "PageOne"), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_DOCUMENTNAME, "MultiDoc"), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_FAXRECVPARAMS, uint32_t{42}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_FAXRECVTIME, uint32_t{19}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_FAXSUBADDRESS, "555-0199"), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_FAXDCS, "DCS-DATA"), 1);

      std::vector<uint8_t> row(16 * 4);
      for (uint32_t y = 0; y < 8; ++y) {
        for (size_t i = 0; i < row.size(); ++i) {
          row[i] = static_cast<uint8_t>((y * 17 + i) & 0xffU);
        }
        ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), y, 0), 1);
      }
      ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
    }

    // Page 2: Deflate tiled palette image with ColorMap.
    {
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{32}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{16}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, uint32_t{16}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, uint32_t{16}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_ADOBE_DEFLATE}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_PALETTE}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COLORMAP, red_map.data(),
                             green_map.data(), blue_map.data()),
                1);
      ASSERT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_PAGENUMBER, uint16_t{1}, uint16_t{8}),
          1);

      std::array<uint8_t, 16 * 16> tile{};
      for (uint32_t t = 0; t < 2; ++t) {
        for (size_t i = 0; i < tile.size(); ++i) {
          tile[i] = static_cast<uint8_t>((i + t * 31) & 0xffU);
        }
        ASSERT_EQ(TIFFWriteEncodedTile(tif.get(), t, tile.data(),
                                       static_cast<tmsize_t>(tile.size())),
                  static_cast<tmsize_t>(tile.size()));
      }
      ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
    }

    // Page 3: JPEG strip page with JPEGTables enabled.
    {
      ASSERT_EQ(TIFFIsCODECConfigured(COMPRESSION_JPEG), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{16}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_JPEG}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_RGB}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{16}), 1);
      std::array<uint8_t, 16 * 3> row{};
      for (uint32_t y = 0; y < 16; ++y) {
        for (size_t i = 0; i < row.size(); ++i) {
          row[i] = static_cast<uint8_t>((y * 11 + i * 3) & 0xffU);
        }
        ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), y, 0), 1);
      }
      ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
    }

    // Page 4: JPEG strip page without JPEGTables (JPEGTABLESMODE = 0).
    {
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{16}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_JPEG}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGTABLESMODE, 0), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_RGB}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{16}), 1);
      std::array<uint8_t, 16 * 3> row{};
      for (uint32_t y = 0; y < 16; ++y) {
        std::fill(row.begin(), row.end(), static_cast<uint8_t>(y * 10));
        ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), y, 0), 1);
      }
      ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
    }

    // Page 5: CCITT Group 3 with Group3Options and fax line tags.
    {
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{8}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_CCITTFAX3}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_MINISWHITE}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{8}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_GROUP3OPTIONS,
                             uint32_t{GROUP3OPT_2DENCODING}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BADFAXLINES, uint32_t{2}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_CLEANFAXDATA,
                             uint16_t{CLEANFAXDATA_REGENERATED}),
                1);
      ASSERT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_CONSECUTIVEBADFAXLINES, uint32_t{1}),
          1);

      std::array<uint8_t, 2> row = {0xAA, 0x55};
      for (uint32_t y = 0; y < 8; ++y) {
        ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), y, 0), 1);
      }
      ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
    }

    // Page 6: CCITT Group 4 with Group4Options.
    {
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{8}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_CCITTFAX4}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_MINISWHITE}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{8}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_GROUP4OPTIONS, uint32_t{0}), 1);

      std::array<uint8_t, 2> row = {0xF0, 0x0F};
      for (uint32_t y = 0; y < 8; ++y) {
        ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), y, 0), 1);
      }
      ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
    }

    // Page 7: 3D tiled image with ImageDepth and TileDepth.
    {
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{16}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEDEPTH, uint32_t{2}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, uint32_t{16}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, uint32_t{16}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEDEPTH, uint32_t{1}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_NONE}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_MINISBLACK}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);

      std::array<uint8_t, 16 * 16> tile{};
      std::fill(tile.begin(), tile.end(), 0x11);
      ASSERT_EQ(TIFFWriteEncodedTile(tif.get(), 0, tile.data(),
                                     static_cast<tmsize_t>(tile.size())),
                static_cast<tmsize_t>(tile.size()));
      std::fill(tile.begin(), tile.end(), 0x22);
      ASSERT_EQ(TIFFWriteEncodedTile(tif.get(), 1, tile.data(),
                                     static_cast<tmsize_t>(tile.size())),
                static_cast<tmsize_t>(tile.size()));
      ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
    }

    // Page 8: Planar separate RGB strip page.
    {
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{8}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_NONE}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_RGB}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_SEPARATE}),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{4}), 1);

      std::array<uint8_t, 8> row{};
      for (uint16_t s = 0; s < 3; ++s) {
        for (uint32_t y = 0; y < 4; ++y) {
          std::fill(row.begin(), row.end(),
                    static_cast<uint8_t>(s * 50 + y * 3 + 7));
          ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), y, s), 1);
        }
      }
      ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
    }

    EXPECT_EQ(diagnostics.error_count(), 0U);
  }

  const std::string prefix = dir.child("page_").native();
  const SubprocessResult res =
      RunSubprocess(TiffsplitBinary(), {in_path.native(), prefix});
  EXPECT_EQ(res.term_signal, 0);
  EXPECT_EQ(res.exit_code, 0);
  EXPECT_THAT(res.stderr_output, IsEmpty());

  const std::vector<std::string> lines =
      absl::StrSplit(res.stdout_output, '\n', absl::SkipEmpty());
  ASSERT_EQ(lines.size(), 8U);
  for (size_t i = 0; i < lines.size(); ++i) {
    EXPECT_EQ(lines[i], absl::StrCat(prefix, "000000", i + 1, ".tif"));
    EXPECT_TRUE(std::filesystem::exists(lines[i]));
  }

  // Verify split Page 1 tags and pixel decoding.
  {
    DiagnosticRecorder diagnostics;
    UniqueTiff out1 = OpenQuietTiff(lines[0], "r", &diagnostics);
    ASSERT_NE(out1.get(), nullptr);
    EXPECT_EQ(TIFFIsBigEndian(out1.get()), 0);
    EXPECT_EQ(TIFFReadDirectory(out1.get()), 0);  // Single-directory output.

    uint32_t subfiletype = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t bps = 0;
    uint16_t spp = 0;
    uint16_t comp = 0;
    uint16_t pred = 0;
    uint16_t photo = 0;
    uint16_t thresh = 0;
    uint16_t fill = 0;
    uint16_t orient = 0;
    uint16_t min_v = 0;
    uint16_t max_v = 0;
    float xres = 0.0f;
    float yres = 0.0f;
    uint16_t resunit = 0;
    float xpos = 0.0f;
    float ypos = 0.0f;
    uint16_t sfmt = 0;
    uint16_t extra_cnt = 0;
    uint16_t* extra_vals = nullptr;
    uint16_t pg_num = 0;
    uint16_t pg_tot = 0;
    char* str_val = nullptr;
    uint32_t fax_params = 0;
    uint32_t fax_time = 0;

    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_SUBFILETYPE, &subfiletype), 1);
    EXPECT_EQ(subfiletype, FILETYPE_PAGE);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_IMAGEWIDTH, &width), 1);
    EXPECT_EQ(width, 16U);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_IMAGELENGTH, &height), 1);
    EXPECT_EQ(height, 8U);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_BITSPERSAMPLE, &bps), 1);
    EXPECT_EQ(bps, 8);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_SAMPLESPERPIXEL, &spp), 1);
    EXPECT_EQ(spp, 4);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_COMPRESSION, &comp), 1);
    EXPECT_EQ(comp, COMPRESSION_LZW);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_PREDICTOR, &pred), 1);
    EXPECT_EQ(pred, PREDICTOR_HORIZONTAL);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_PHOTOMETRIC, &photo), 1);
    EXPECT_EQ(photo, PHOTOMETRIC_RGB);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_THRESHHOLDING, &thresh), 1);
    EXPECT_EQ(thresh, THRESHHOLD_BILEVEL);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_FILLORDER, &fill), 1);
    EXPECT_EQ(fill, FILLORDER_MSB2LSB);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_ORIENTATION, &orient), 1);
    EXPECT_EQ(orient, ORIENTATION_TOPLEFT);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_MINSAMPLEVALUE, &min_v), 1);
    EXPECT_EQ(min_v, 2);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_MAXSAMPLEVALUE, &max_v), 1);
    EXPECT_EQ(max_v, 250);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_XRESOLUTION, &xres), 1);
    EXPECT_FLOAT_EQ(xres, 300.0f);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_YRESOLUTION, &yres), 1);
    EXPECT_FLOAT_EQ(yres, 150.0f);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_RESOLUTIONUNIT, &resunit), 1);
    EXPECT_EQ(resunit, RESUNIT_INCH);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_XPOSITION, &xpos), 1);
    EXPECT_FLOAT_EQ(xpos, 1.25f);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_YPOSITION, &ypos), 1);
    EXPECT_FLOAT_EQ(ypos, 2.5f);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_SAMPLEFORMAT, &sfmt), 1);
    EXPECT_EQ(sfmt, SAMPLEFORMAT_UINT);
    EXPECT_EQ(
        TIFFGetField(out1.get(), TIFFTAG_EXTRASAMPLES, &extra_cnt, &extra_vals),
        1);
    ASSERT_EQ(extra_cnt, 1);
    ASSERT_NE(extra_vals, nullptr);
    EXPECT_EQ(extra_vals[0], EXTRASAMPLE_ASSOCALPHA);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_PAGENUMBER, &pg_num, &pg_tot),
              1);
    EXPECT_EQ(pg_num, 0);
    EXPECT_EQ(pg_tot, 8);

    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_ARTIST, &str_val), 1);
    EXPECT_STREQ(str_val, "Test Artist");
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_IMAGEDESCRIPTION, &str_val), 1);
    EXPECT_STREQ(str_val, "LZW RGBA strip page");
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_MAKE, &str_val), 1);
    EXPECT_STREQ(str_val, "TestMake");
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_MODEL, &str_val), 1);
    EXPECT_STREQ(str_val, "TestModel");
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_SOFTWARE, &str_val), 1);
    EXPECT_STREQ(str_val, "TestSoft");
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_DATETIME, &str_val), 1);
    EXPECT_STREQ(str_val, "2026:11:10 01:30:00");
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_HOSTCOMPUTER, &str_val), 1);
    EXPECT_STREQ(str_val, "TestHost");
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_PAGENAME, &str_val), 1);
    EXPECT_STREQ(str_val, "PageOne");
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_DOCUMENTNAME, &str_val), 1);
    EXPECT_STREQ(str_val, "MultiDoc");
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_FAXRECVPARAMS, &fax_params), 1);
    EXPECT_EQ(fax_params, 42U);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_FAXRECVTIME, &fax_time), 1);
    EXPECT_EQ(fax_time, 19U);
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_FAXSUBADDRESS, &str_val), 1);
    EXPECT_STREQ(str_val, "555-0199");
    EXPECT_EQ(TIFFGetField(out1.get(), TIFFTAG_FAXDCS, &str_val), 1);
    EXPECT_STREQ(str_val, "DCS-DATA");

    std::vector<uint8_t> row(16 * 4);
    for (uint32_t y = 0; y < 8; ++y) {
      ASSERT_EQ(TIFFReadScanline(out1.get(), row.data(), y, 0), 1);
      for (size_t i = 0; i < row.size(); ++i) {
        EXPECT_EQ(row[i], static_cast<uint8_t>((y * 17 + i) & 0xffU));
      }
    }
    EXPECT_EQ(diagnostics.error_count(), 0U);
  }

  // Verify split Page 2 (tiled + ColorMap).
  {
    DiagnosticRecorder diagnostics;
    UniqueTiff out2 = OpenQuietTiff(lines[1], "r", &diagnostics);
    ASSERT_NE(out2.get(), nullptr);
    EXPECT_EQ(TIFFIsTiled(out2.get()), 1);
    uint16_t* r = nullptr;
    uint16_t* g = nullptr;
    uint16_t* b = nullptr;
    ASSERT_EQ(TIFFGetField(out2.get(), TIFFTAG_COLORMAP, &r, &g, &b), 1);
    EXPECT_EQ(r[10], red_map[10]);
    EXPECT_EQ(g[10], green_map[10]);
    EXPECT_EQ(b[10], blue_map[10]);

    std::array<uint8_t, 16 * 16> tile{};
    ASSERT_EQ(TIFFReadEncodedTile(out2.get(), 1, tile.data(),
                                  static_cast<tmsize_t>(tile.size())),
              static_cast<tmsize_t>(tile.size()));
    EXPECT_EQ(tile[0], static_cast<uint8_t>(31));
    EXPECT_EQ(diagnostics.error_count(), 0U);
  }

  // Verify split Page 3 (JPEG + JPEGTables) and Page 4 (JPEG without tables).
  {
    DiagnosticRecorder diagnostics;
    UniqueTiff out3 = OpenQuietTiff(lines[2], "r", &diagnostics);
    ASSERT_NE(out3.get(), nullptr);
    uint32_t jt_count = 0;
    void* jt_ptr = nullptr;
    EXPECT_EQ(TIFFGetField(out3.get(), TIFFTAG_JPEGTABLES, &jt_count, &jt_ptr),
              1);
    EXPECT_GT(jt_count, 0U);
    EXPECT_NE(jt_ptr, nullptr);
    std::array<uint8_t, 16 * 3> jpeg_row{};
    EXPECT_EQ(TIFFReadScanline(out3.get(), jpeg_row.data(), 0, 0), 1);

    UniqueTiff out4 = OpenQuietTiff(lines[3], "r", &diagnostics);
    ASSERT_NE(out4.get(), nullptr);
    EXPECT_EQ(TIFFGetField(out4.get(), TIFFTAG_JPEGTABLES, &jt_count, &jt_ptr),
              0);
    EXPECT_EQ(TIFFReadScanline(out4.get(), jpeg_row.data(), 0, 0), 1);
    EXPECT_EQ(diagnostics.error_count(), 0U);
  }

  // Verify split Page 5 (G3) & Page 6 (G4) & Page 7 (3D tile depth).
  {
    DiagnosticRecorder diagnostics;
    UniqueTiff out5 = OpenQuietTiff(lines[4], "r", &diagnostics);
    ASSERT_NE(out5.get(), nullptr);
    uint32_t g3_opts = 0;
    uint32_t bad_lines = 0;
    uint16_t clean_fax = 0;
    uint32_t consec_bad = 0;
    EXPECT_EQ(TIFFGetField(out5.get(), TIFFTAG_GROUP3OPTIONS, &g3_opts), 1);
    EXPECT_EQ(g3_opts, uint32_t{GROUP3OPT_2DENCODING});
    EXPECT_EQ(TIFFGetField(out5.get(), TIFFTAG_BADFAXLINES, &bad_lines), 1);
    EXPECT_EQ(bad_lines, 2U);
    EXPECT_EQ(TIFFGetField(out5.get(), TIFFTAG_CLEANFAXDATA, &clean_fax), 1);
    EXPECT_EQ(clean_fax, CLEANFAXDATA_REGENERATED);
    EXPECT_EQ(
        TIFFGetField(out5.get(), TIFFTAG_CONSECUTIVEBADFAXLINES, &consec_bad),
        1);
    EXPECT_EQ(consec_bad, 1U);
    std::array<uint8_t, 2> fax_row{};
    ASSERT_EQ(TIFFReadScanline(out5.get(), fax_row.data(), 0, 0), 1);
    EXPECT_EQ(fax_row[0], 0xAA);
    EXPECT_EQ(fax_row[1], 0x55);

    UniqueTiff out6 = OpenQuietTiff(lines[5], "r", &diagnostics);
    ASSERT_NE(out6.get(), nullptr);
    uint32_t g4_opts = 99;
    EXPECT_EQ(TIFFGetField(out6.get(), TIFFTAG_GROUP4OPTIONS, &g4_opts), 1);
    EXPECT_EQ(g4_opts, 0U);
    ASSERT_EQ(TIFFReadScanline(out6.get(), fax_row.data(), 0, 0), 1);
    EXPECT_EQ(fax_row[0], 0xF0);
    EXPECT_EQ(fax_row[1], 0x0F);

    UniqueTiff out7 = OpenQuietTiff(lines[6], "r", &diagnostics);
    ASSERT_NE(out7.get(), nullptr);
    uint32_t img_depth = 0;
    uint32_t tile_depth = 0;
    EXPECT_EQ(TIFFGetField(out7.get(), TIFFTAG_IMAGEDEPTH, &img_depth), 1);
    EXPECT_EQ(img_depth, 2U);
    EXPECT_EQ(TIFFGetField(out7.get(), TIFFTAG_TILEDEPTH, &tile_depth), 1);
    EXPECT_EQ(tile_depth, 1U);
    std::array<uint8_t, 16 * 16> tile{};
    ASSERT_EQ(TIFFReadEncodedTile(out7.get(), 1, tile.data(),
                                  static_cast<tmsize_t>(tile.size())),
              static_cast<tmsize_t>(tile.size()));
    EXPECT_EQ(tile[0], 0x22);

    UniqueTiff out8 = OpenQuietTiff(lines[7], "r", &diagnostics);
    ASSERT_NE(out8.get(), nullptr);
    uint16_t planar = 0;
    EXPECT_EQ(TIFFGetField(out8.get(), TIFFTAG_PLANARCONFIG, &planar), 1);
    EXPECT_EQ(planar, PLANARCONFIG_SEPARATE);
    std::array<uint8_t, 8> sep_row{};
    for (uint16_t s = 0; s < 3; ++s) {
      for (uint32_t y = 0; y < 4; ++y) {
        ASSERT_EQ(TIFFReadScanline(out8.get(), sep_row.data(), y, s), 1);
        EXPECT_EQ(sep_row[0], static_cast<uint8_t>(s * 50 + y * 3 + 7));
      }
    }
    EXPECT_EQ(diagnostics.error_count(), 0U);
  }
}

TEST(TiffsplitTest, BigEndianInputPreservesBigEndianInOutputs) {
  TempDir dir = MakeTestDir();
  const std::filesystem::path in_path = dir.child("big_endian.tif");
  {
    DiagnosticRecorder diagnostics;
    UniqueTiff tif = OpenQuietTiff(in_path, "wb", &diagnostics);
    ASSERT_NE(tif.get(), nullptr);
    WriteSimpleStripPage(tif.get(), 8, 4, 5);
    WriteSimpleTilePage(tif.get(), 16, 16, 16, 16, 20);
    EXPECT_EQ(diagnostics.error_count(), 0U);
  }

  const std::string prefix = dir.child("be_").native();
  const SubprocessResult res =
      RunSubprocess(TiffsplitBinary(), {in_path.native(), prefix});
  EXPECT_EQ(res.term_signal, 0);
  EXPECT_EQ(res.exit_code, 0);
  EXPECT_THAT(res.stderr_output, IsEmpty());

  const std::vector<std::string> lines =
      absl::StrSplit(res.stdout_output, '\n', absl::SkipEmpty());
  ASSERT_EQ(lines.size(), 2U);
  {
    DiagnosticRecorder diagnostics;
    UniqueTiff out1 = OpenQuietTiff(lines[0], "r", &diagnostics);
    ASSERT_NE(out1.get(), nullptr);
    EXPECT_EQ(TIFFIsBigEndian(out1.get()), 1);
    std::array<uint8_t, 8> row{};
    ASSERT_EQ(TIFFReadScanline(out1.get(), row.data(), 0, 0), 1);
    EXPECT_EQ(row[0], 5);

    UniqueTiff out2 = OpenQuietTiff(lines[1], "r", &diagnostics);
    ASSERT_NE(out2.get(), nullptr);
    EXPECT_EQ(TIFFIsBigEndian(out2.get()), 1);
    std::array<uint8_t, 16 * 16> tile{};
    ASSERT_EQ(TIFFReadEncodedTile(out2.get(), 0, tile.data(),
                                  static_cast<tmsize_t>(tile.size())),
              static_cast<tmsize_t>(tile.size()));
    EXPECT_EQ(tile[0], 20);
    EXPECT_EQ(diagnostics.error_count(), 0U);
  }
}

TEST(TiffsplitTest, BigTiffInputSplitsCleanly) {
  TempDir dir = MakeTestDir();
  const std::filesystem::path in_path = dir.child("bigtiff.tif");
  {
    DiagnosticRecorder diagnostics;
    UniqueTiff tif = OpenQuietTiff(in_path, "w8", &diagnostics);
    ASSERT_NE(tif.get(), nullptr);
    WriteSimpleStripPage(tif.get(), 8, 8, 1);
    WriteSimpleTilePage(tif.get(), 32, 16, 16, 16, 40);
    EXPECT_EQ(diagnostics.error_count(), 0U);
  }

  const std::string prefix = dir.child("bt_").native();
  const SubprocessResult res =
      RunSubprocess(TiffsplitBinary(), {"-M", "0", in_path.native(), prefix});
  EXPECT_EQ(res.term_signal, 0);
  EXPECT_EQ(res.exit_code, 0);
  EXPECT_THAT(res.stderr_output, IsEmpty());

  const std::vector<std::string> lines =
      absl::StrSplit(res.stdout_output, '\n', absl::SkipEmpty());
  ASSERT_EQ(lines.size(), 2U);

  DiagnosticRecorder diagnostics;
  UniqueTiff out1 = OpenQuietTiff(lines[0], "r", &diagnostics);
  ASSERT_NE(out1.get(), nullptr);
  std::array<uint8_t, 8> row{};
  ASSERT_EQ(TIFFReadScanline(out1.get(), row.data(), 0, 0), 1);
  EXPECT_EQ(row[0], 1);

  UniqueTiff out2 = OpenQuietTiff(lines[1], "r", &diagnostics);
  ASSERT_NE(out2.get(), nullptr);
  std::array<uint8_t, 16 * 16> tile{};
  ASSERT_EQ(TIFFReadEncodedTile(out2.get(), 1, tile.data(),
                                static_cast<tmsize_t>(tile.size())),
            static_cast<tmsize_t>(tile.size()));
  EXPECT_EQ(tile[0], 47);
  EXPECT_EQ(diagnostics.error_count(), 0U);
}

TEST(TiffsplitTest, StripAndTileReallocSucceedsWhenByteCountsExceedEstimate) {
  TempDir dir = MakeTestDir();
  const std::filesystem::path in_path = dir.child("realloc_ok.tif");

  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  // Page 1: strip image where StripByteCounts[0]=2, StripByteCounts[1]=64
  // while TIFFStripSize(in) is small (width=1, rowsperstrip=1, PackBits).
  std::array<uint8_t, 2> s0 = {0x00, 0x11};
  std::array<uint8_t, 64> s1{};
  std::fill(s1.begin(), s1.end(), 0x22);
  const uint64_t s0_off = builder.AppendBlob(s0);
  const uint64_t s1_off = builder.AppendBlob(s1);
  const std::array<uint32_t, 2> strip_offs = {static_cast<uint32_t>(s0_off),
                                              static_cast<uint32_t>(s1_off)};
  const std::array<uint32_t, 2> strip_cnts = {2U, 64U};
  builder.AddIfd({
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, 1),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, 2),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, strip_offs),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
      builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, strip_cnts),
  });

  // Page 2: tiled image where TileByteCounts[1]=512 exceeds TIFFTileSize (256).
  std::array<uint8_t, 16> t0{};
  std::fill(t0.begin(), t0.end(), 0x33);
  std::array<uint8_t, 512> t1{};
  std::fill(t1.begin(), t1.end(), 0x44);
  const uint64_t t0_off = builder.AppendBlob(t0);
  const uint64_t t1_off = builder.AppendBlob(t1);
  const std::array<uint32_t, 2> tile_offs = {static_cast<uint32_t>(t0_off),
                                             static_cast<uint32_t>(t1_off)};
  const std::array<uint32_t, 2> tile_cnts = {16U, 512U};
  builder.AddIfd({
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, 32),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeLong(TIFFTAG_TILEWIDTH, 16),
      builder.MakeLong(TIFFTAG_TILELENGTH, 16),
      builder.MakeLongArray(TIFFTAG_TILEOFFSETS, tile_offs),
      builder.MakeLongArray(TIFFTAG_TILEBYTECOUNTS, tile_cnts),
  });

  ASSERT_TRUE(WriteBytesToFile(in_path, builder.bytes()));
  const std::string prefix = dir.child("re_").native();
  const SubprocessResult res =
      RunSubprocess(TiffsplitBinary(), {"-M", "1", in_path.native(), prefix});
  EXPECT_EQ(res.term_signal, 0);
  EXPECT_EQ(res.exit_code, 0);
  EXPECT_THAT(res.stderr_output, IsEmpty());

  const std::vector<std::string> lines =
      absl::StrSplit(res.stdout_output, '\n', absl::SkipEmpty());
  ASSERT_EQ(lines.size(), 2U);

  DiagnosticRecorder diagnostics;
  UniqueTiff out1 = OpenQuietTiff(lines[0], "r", &diagnostics);
  ASSERT_NE(out1.get(), nullptr);
  std::array<uint8_t, 64> raw_strip{};
  EXPECT_EQ(TIFFReadRawStrip(out1.get(), 1, raw_strip.data(), 64), 64);
  EXPECT_EQ(raw_strip[0], 0x22);

  UniqueTiff out2 = OpenQuietTiff(lines[1], "r", &diagnostics);
  ASSERT_NE(out2.get(), nullptr);
  std::array<uint8_t, 512> raw_tile{};
  EXPECT_EQ(TIFFReadRawTile(out2.get(), 1, raw_tile.data(), 512), 512);
  EXPECT_EQ(raw_tile[0], 0x44);
  EXPECT_EQ(diagnostics.error_count(), 0U);
}

TEST(TiffsplitTest, MemoryLimitRejectsLargeStripAndTileAllocations) {
  TempDir dir = MakeTestDir();
  constexpr uint32_t kOverOneMiB = (1024U * 1024U) + 16U;

  // Case 1: cpStrips limitMalloc failure (TIFFStripSize > 1 MiB).
  {
    const std::filesystem::path strip_alloc_path =
        dir.child("strip_alloc_limit.tif");
    RawTiffBuilder builder(false, false);
    std::vector<uint8_t> dummy(16, 0xAA);
    const uint64_t off = builder.AppendBlob(dummy);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, kOverOneMiB),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 1),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 16),
    });
    ASSERT_TRUE(WriteBytesToFile(strip_alloc_path, builder.bytes()));
    const SubprocessResult res = RunSubprocess(
        TiffsplitBinary(),
        {"-M", "1", strip_alloc_path.native(), dir.child("sa_").native()});
    EXPECT_EQ(res.term_signal, 0);
    EXPECT_EQ(res.exit_code, 1);
    EXPECT_THAT(res.stderr_output, HasSubstr("MemoryLimitError: allocation"));
    EXPECT_THAT(res.stderr_output, HasSubstr("bytes for strip-size"));
    EXPECT_THAT(res.stderr_output,
                HasSubstr("Could not copy data from input to output"));
  }

  // Case 2: cpTiles limitMalloc failure (TIFFTileSize > 1 MiB).
  {
    const std::filesystem::path tile_alloc_path =
        dir.child("tile_alloc_limit.tif");
    RawTiffBuilder builder(false, false);
    std::vector<uint8_t> dummy(16, 0xBB);
    const uint64_t off = builder.AppendBlob(dummy);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 1024),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 1056),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_TILEWIDTH, 1024),
        builder.MakeLong(TIFFTAG_TILELENGTH, 1056),
        builder.MakeLong(TIFFTAG_TILEOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeLong(TIFFTAG_TILEBYTECOUNTS, 16),
    });
    ASSERT_TRUE(WriteBytesToFile(tile_alloc_path, builder.bytes()));
    const SubprocessResult res = RunSubprocess(
        TiffsplitBinary(),
        {"-M", "1", tile_alloc_path.native(), dir.child("ta_").native()});
    EXPECT_EQ(res.term_signal, 0);
    EXPECT_EQ(res.exit_code, 1);
    EXPECT_THAT(res.stderr_output, HasSubstr("MemoryLimitError: allocation"));
    EXPECT_THAT(res.stderr_output, HasSubstr("bytes for tile-size"));
  }

  // Case 3: cpStrips limitRealloc failure.
  {
    const std::filesystem::path strip_realloc_path =
        dir.child("strip_realloc_limit.tif");
    RawTiffBuilder builder(false, false);
    std::vector<uint8_t> s0(8, 0x11);
    const uint64_t off0 = builder.AppendBlob(s0);
    const std::array<uint32_t, 2> offs = {static_cast<uint32_t>(off0),
                                          static_cast<uint32_t>(off0)};
    const std::array<uint32_t, 2> cnts = {8U, kOverOneMiB};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 2),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offs),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, cnts),
    });
    ASSERT_TRUE(WriteBytesToFile(strip_realloc_path, builder.bytes()));
    const SubprocessResult res = RunSubprocess(
        TiffsplitBinary(),
        {"-M", "1", strip_realloc_path.native(), dir.child("sr_").native()});
    EXPECT_EQ(res.term_signal, 0);
    EXPECT_EQ(res.exit_code, 1);
    EXPECT_THAT(res.stderr_output,
                HasSubstr("MemoryLimitError: re-allocation"));
    EXPECT_THAT(res.stderr_output, HasSubstr("Can't re-allocate"));
    EXPECT_THAT(res.stderr_output, HasSubstr("bytes for strip-size"));
  }

  // Case 4: cpTiles limitRealloc failure.
  {
    const std::filesystem::path tile_realloc_path =
        dir.child("tile_realloc_limit.tif");
    RawTiffBuilder builder(false, false);
    std::vector<uint8_t> t0(8, 0x22);
    const uint64_t off0 = builder.AppendBlob(t0);
    const std::array<uint32_t, 2> offs = {static_cast<uint32_t>(off0),
                                          static_cast<uint32_t>(off0)};
    const std::array<uint32_t, 2> cnts = {8U, kOverOneMiB};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 32),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_TILEWIDTH, 16),
        builder.MakeLong(TIFFTAG_TILELENGTH, 16),
        builder.MakeLongArray(TIFFTAG_TILEOFFSETS, offs),
        builder.MakeLongArray(TIFFTAG_TILEBYTECOUNTS, cnts),
    });
    ASSERT_TRUE(WriteBytesToFile(tile_realloc_path, builder.bytes()));
    const SubprocessResult res = RunSubprocess(
        TiffsplitBinary(),
        {"-M", "1", tile_realloc_path.native(), dir.child("tr_").native()});
    EXPECT_EQ(res.term_signal, 0);
    EXPECT_EQ(res.exit_code, 1);
    EXPECT_THAT(res.stderr_output,
                HasSubstr("MemoryLimitError: re-allocation"));
    EXPECT_THAT(res.stderr_output, HasSubstr("bytes for tile-size"));
  }
}

TEST(TiffsplitTest, TruncatedStripAndTileStreamsFailCleanly) {
  TempDir dir = MakeTestDir();

  // Truncated strip data: StripOffsets past EOF.
  {
    const std::filesystem::path path = dir.child("trunc_strip.tif");
    RawTiffBuilder builder(false, false);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_LZW),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, 100000),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 16),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 256),
    });
    ASSERT_TRUE(WriteBytesToFile(path, builder.bytes()));

    const SubprocessResult res = RunSubprocess(
        TiffsplitBinary(), {path.native(), dir.child("ts_").native()});
    EXPECT_EQ(res.term_signal, 0);
    EXPECT_EQ(res.exit_code, 1);
    EXPECT_THAT(res.stderr_output,
                HasSubstr("Could not copy data from input to output"));
  }

  // Truncated tile data: TileOffsets past EOF.
  {
    const std::filesystem::path path = dir.child("trunc_tile.tif");
    RawTiffBuilder builder(false, false);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_LZW),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_TILEWIDTH, 16),
        builder.MakeLong(TIFFTAG_TILELENGTH, 16),
        builder.MakeLong(TIFFTAG_TILEOFFSETS, 100000),
        builder.MakeLong(TIFFTAG_TILEBYTECOUNTS, 256),
    });
    ASSERT_TRUE(WriteBytesToFile(path, builder.bytes()));

    const SubprocessResult res = RunSubprocess(
        TiffsplitBinary(), {path.native(), dir.child("tt_").native()});
    EXPECT_EQ(res.term_signal, 0);
    EXPECT_EQ(res.exit_code, 1);
    EXPECT_THAT(res.stderr_output,
                HasSubstr("Could not copy data from input to output"));
  }
}

TEST(TiffsplitTest, MissingStripAndTileByteCountsFailCleanly) {
  TempDir dir = MakeTestDir();

  // OJPEG compression skips EstimateStripByteCounts in tif_dirread.c, leaving
  // TIFFTAG_STRIPBYTECOUNTS / TIFFTAG_TILEBYTECOUNTS unset.
  {
    const std::filesystem::path path = dir.child("no_strip_bc.tif");
    RawTiffBuilder builder(false, false);
    const std::array<uint8_t, 4> data = {1, 2, 3, 4};
    const uint64_t off = builder.AppendBlob(data);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 2),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 2),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_OJPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 2),
    });
    ASSERT_TRUE(WriteBytesToFile(path, builder.bytes()));

    const SubprocessResult res = RunSubprocess(
        TiffsplitBinary(), {path.native(), dir.child("nsbc_").native()});
    EXPECT_EQ(res.term_signal, 0);
    EXPECT_EQ(res.exit_code, 1);
    EXPECT_THAT(res.stderr_output, HasSubstr("strip byte counts are missing"));
  }

  {
    const std::filesystem::path path = dir.child("no_tile_bc.tif");
    RawTiffBuilder builder(false, false);
    const std::array<uint8_t, 16> data{};
    const uint64_t off = builder.AppendBlob(data);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_OJPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_TILEWIDTH, 16),
        builder.MakeLong(TIFFTAG_TILELENGTH, 16),
        builder.MakeLong(TIFFTAG_TILEOFFSETS, static_cast<uint32_t>(off)),
    });
    ASSERT_TRUE(WriteBytesToFile(path, builder.bytes()));

    const SubprocessResult res = RunSubprocess(
        TiffsplitBinary(), {path.native(), dir.child("ntbc_").native()});
    EXPECT_EQ(res.term_signal, 0);
    EXPECT_EQ(res.exit_code, 1);
    EXPECT_THAT(res.stderr_output, HasSubstr("tile byte counts are missing"));
  }
}

}  // namespace
}  // namespace libtiff
