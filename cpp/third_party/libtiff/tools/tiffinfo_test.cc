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

// Clean-room unit and subprocess tests for third_party/tiff/tools/tiffinfo.c.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT(build/c++17)
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/temp_file.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

std::filesystem::path TiffinfoExecutable() {
  return ResolveRunfilesPath("third_party/tiff/tiffinfo");
}

SubprocessResult RunTiffinfo(const std::vector<std::string>& args) {
  return RunSubprocess(TiffinfoExecutable(), args);
}

void WriteBytesToFile(const std::filesystem::path& path,
                      absl::Span<const uint8_t> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << "Failed to open " << path;
  if (!bytes.empty()) {
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  out.flush();
  ASSERT_TRUE(out.good()) << "Failed writing " << path;
}

UniqueTiff OpenForWrite(const std::filesystem::path& path, const char* mode,
                        DiagnosticRecorder& diagnostics) {
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  if (opts == nullptr) return nullptr;
  diagnostics.Install(opts.get());
  return UniqueTiff(TIFFOpenExt(path.c_str(), mode, opts.get()));
}

void WriteMinimalStripedGray8(const std::filesystem::path& path,
                              const char* mode, uint32_t width, uint32_t height,
                              uint32_t rows_per_strip, uint16_t fill_order,
                              uint16_t compression, uint8_t seed) {
  DiagnosticRecorder diag;
  UniqueTiff tif = OpenForWrite(path, mode, diag);
  ASSERT_NE(tif.get(), nullptr);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, compression), 1);
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
            1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_FILLORDER, fill_order), 1);

  std::vector<uint8_t> row(width);
  for (uint32_t r = 0; r < height; ++r) {
    for (uint32_t c = 0; c < width; ++c) {
      row[c] = static_cast<uint8_t>(seed + r * width + c);
    }
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), r, 0), 1);
  }
  tif.reset();
  EXPECT_EQ(diag.error_count(), 0U);
}

TEST(TiffinfoTest, HelpAndInvalidArguments) {
  const SubprocessResult help = RunTiffinfo({"-h"});
  EXPECT_EQ(help.term_signal, 0);
  EXPECT_EQ(help.exit_code, 0);
  EXPECT_THAT(help.stdout_output, HasSubstr("LIBTIFF, Version"));
  EXPECT_THAT(help.stdout_output,
              HasSubstr("usage: tiffinfo [options] input..."));
  EXPECT_THAT(help.stderr_output, IsEmpty());

  const SubprocessResult no_args = RunTiffinfo({});
  EXPECT_EQ(no_args.term_signal, 0);
  EXPECT_EQ(no_args.exit_code, 1);
  EXPECT_THAT(no_args.stdout_output, IsEmpty());
  EXPECT_THAT(no_args.stderr_output,
              HasSubstr("usage: tiffinfo [options] input..."));

  const SubprocessResult unknown_opt = RunTiffinfo({"-Z"});
  EXPECT_EQ(unknown_opt.term_signal, 0);
  EXPECT_EQ(unknown_opt.exit_code, 1);
  EXPECT_THAT(unknown_opt.stdout_output, IsEmpty());
  EXPECT_THAT(unknown_opt.stderr_output, HasSubstr("usage: tiffinfo"));

  const SubprocessResult bad_fill = RunTiffinfo({"-f", "bogus", "file.tif"});
  EXPECT_EQ(bad_fill.term_signal, 0);
  EXPECT_EQ(bad_fill.exit_code, 1);
  EXPECT_THAT(bad_fill.stdout_output, IsEmpty());
  EXPECT_THAT(bad_fill.stderr_output, HasSubstr("usage: tiffinfo"));

  for (const char* bad_mem : {"-1", "not_a_number"}) {
    SCOPED_TRACE(bad_mem);
    const SubprocessResult res = RunTiffinfo({"-M", bad_mem, "file.tif"});
    EXPECT_EQ(res.term_signal, 0);
    EXPECT_EQ(res.exit_code, 1);
    EXPECT_THAT(res.stdout_output, IsEmpty());
    EXPECT_THAT(res.stderr_output, HasSubstr("usage: tiffinfo"));
  }

  const SubprocessResult neg_off = RunTiffinfo({"-o", "-1", "file.tif"});
  EXPECT_EQ(neg_off.term_signal, 0);
  EXPECT_EQ(neg_off.exit_code, 1);
  EXPECT_THAT(neg_off.stdout_output, IsEmpty());
  EXPECT_THAT(neg_off.stderr_output, HasSubstr("usage: tiffinfo"));

  // Options 'S', 'l', 'm', and 'v' appear in tiffinfo's getopt string without
  // explicit switch cases, exercising the default branch.
  TempFile file("ignored_opts.tif");
  WriteMinimalStripedGray8(file.path(), "wl", 2, 2, 2, FILLORDER_MSB2LSB,
                           COMPRESSION_NONE, 0x10);
  const SubprocessResult ignored_opts =
      RunTiffinfo({"-S", "-l", "-m", "-v", file.string()});
  EXPECT_EQ(ignored_opts.term_signal, 0);
  EXPECT_EQ(ignored_opts.exit_code, 0);
  EXPECT_THAT(ignored_opts.stdout_output, HasSubstr("Image Width: 2"));
  EXPECT_THAT(ignored_opts.stderr_output, IsEmpty());
}

TEST(TiffinfoTest, MissingEmptyAndCorruptFiles) {
  TempDir dir("missing_corrupt");

  const std::filesystem::path nonexistent = dir.child("no_such_file.tif");
  const SubprocessResult missing = RunTiffinfo({nonexistent.native()});
  EXPECT_EQ(missing.term_signal, 0);
  EXPECT_EQ(missing.exit_code, 1);
  EXPECT_THAT(missing.stdout_output, IsEmpty());
  EXPECT_THAT(missing.stderr_output, Not(IsEmpty()));

  const std::filesystem::path empty_file = dir.child("empty.tif");
  WriteBytesToFile(empty_file, {});
  const SubprocessResult empty_res = RunTiffinfo({empty_file.native()});
  EXPECT_EQ(empty_res.term_signal, 0);
  EXPECT_EQ(empty_res.exit_code, 1);
  EXPECT_THAT(empty_res.stderr_output, Not(IsEmpty()));

  const std::filesystem::path garbage_file = dir.child("garbage.tif");
  const std::array<uint8_t, 12> garbage = {'B', 'A', 'D', '!', 0, 1,
                                           2,   3,   4,   5,   6, 7};
  WriteBytesToFile(garbage_file, garbage);
  const SubprocessResult garbage_res = RunTiffinfo({garbage_file.native()});
  EXPECT_EQ(garbage_res.term_signal, 0);
  EXPECT_EQ(garbage_res.exit_code, 1);
  EXPECT_THAT(garbage_res.stderr_output, Not(IsEmpty()));

  const std::filesystem::path trunc_ifd_file = dir.child("trunc_ifd.tif");
  const std::array<uint8_t, 10> trunc_ifd = {'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0};
  WriteBytesToFile(trunc_ifd_file, trunc_ifd);
  const SubprocessResult trunc_res = RunTiffinfo({trunc_ifd_file.native()});
  EXPECT_EQ(trunc_res.term_signal, 0);
  EXPECT_EQ(trunc_res.exit_code, 1);
  EXPECT_THAT(trunc_res.stderr_output, Not(IsEmpty()));

  // Valid second file still gets processed after an invalid first file.
  const std::filesystem::path valid_file = dir.child("valid.tif");
  WriteMinimalStripedGray8(valid_file, "wl", 4, 2, 2, FILLORDER_MSB2LSB,
                           COMPRESSION_NONE, 0x20);
  const SubprocessResult mixed =
      RunTiffinfo({nonexistent.native(), valid_file.native()});
  EXPECT_EQ(mixed.term_signal, 0);
  EXPECT_EQ(mixed.exit_code, 1);
  EXPECT_THAT(mixed.stdout_output,
              HasSubstr(absl::StrCat("File ", nonexistent.native(), ":\n")));
  EXPECT_THAT(mixed.stdout_output,
              HasSubstr(absl::StrCat("File ", valid_file.native(), ":\n")));
  EXPECT_THAT(mixed.stdout_output, HasSubstr("Image Width: 4 Image Length: 2"));
}

TEST(TiffinfoTest, MultipleFilesEndiannessAndBigTiff) {
  TempDir dir("endian_bigtiff");
  struct FormatCase {
    const char* name;
    const char* mode;
    uint32_t width;
  };
  constexpr auto kCases = std::to_array<FormatCase>({
      {.name = "classic_le.tif", .mode = "wl", .width = 3},
      {.name = "classic_be.tif", .mode = "wb", .width = 5},
      {.name = "bigtiff_le.tif", .mode = "w8l", .width = 7},
      {.name = "bigtiff_be.tif", .mode = "w8b", .width = 9},
  });

  std::vector<std::string> args = {"-D"};
  for (const FormatCase& tc : kCases) {
    const std::filesystem::path p = dir.child(tc.name);
    WriteMinimalStripedGray8(p, tc.mode, tc.width, 2, 2, FILLORDER_MSB2LSB,
                             COMPRESSION_NONE, 0x11);
    args.push_back(p.native());
  }

  const SubprocessResult res = RunTiffinfo(args);
  EXPECT_EQ(res.term_signal, 0);
  EXPECT_EQ(res.exit_code, 0);
  EXPECT_THAT(res.stderr_output, IsEmpty());
  for (const FormatCase& tc : kCases) {
    EXPECT_THAT(res.stdout_output, HasSubstr(tc.name));
    EXPECT_THAT(res.stdout_output,
                HasSubstr(absl::StrFormat("Image Width: %u Image Length: 2",
                                          tc.width)));
  }
}

TEST(TiffinfoTest, MultiPageDirectoryAndOffsetSelection) {
  TempFile file("multipage.tif");
  std::vector<uint64_t> dir_offsets;
  {
    DiagnosticRecorder diag;
    UniqueTiff tif = OpenForWrite(file.path(), "wl", diag);
    ASSERT_NE(tif.get(), nullptr);
    constexpr uint16_t kPageCount = 12;
    for (uint16_t p = 0; p < kPageCount; ++p) {
      const uint32_t w = 2 + p;
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, w), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 1), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
                1);
      ASSERT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
          1);
      ASSERT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 1), 1);
      std::vector<uint8_t> row(w, static_cast<uint8_t>(0x30 + p));
      ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), 1);
      ASSERT_EQ(TIFFCheckpointDirectory(tif.get()), 1);
      dir_offsets.push_back(TIFFCurrentDirOffset(tif.get()));
      ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
    }
    tif.reset();
    EXPECT_EQ(diag.error_count(), 0U);
  }
  ASSERT_EQ(dir_offsets.size(), 12U);

  // Default prints all directories with headers.
  const SubprocessResult all_dirs = RunTiffinfo({file.string()});
  EXPECT_EQ(all_dirs.term_signal, 0);
  EXPECT_EQ(all_dirs.exit_code, 0);
  EXPECT_THAT(all_dirs.stderr_output, IsEmpty());
  for (int p = 0; p < 12; ++p) {
    EXPECT_THAT(all_dirs.stdout_output,
                HasSubstr(absl::StrFormat("=== TIFF directory %d ===", p)));
  }

  // Test -0 through -9 and multi-digit -11.
  for (int d : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11}) {
    SCOPED_TRACE(d);
    const SubprocessResult single =
        RunTiffinfo({absl::StrFormat("-%d", d), file.string()});
    EXPECT_EQ(single.term_signal, 0);
    EXPECT_EQ(single.exit_code, 0);
    EXPECT_THAT(single.stderr_output, IsEmpty());
    EXPECT_THAT(
        single.stdout_output,
        HasSubstr(absl::StrFormat("Image Width: %d Image Length: 1", 2 + d)));
    EXPECT_THAT(single.stdout_output, Not(HasSubstr("=== TIFF directory")));
  }

  // Out-of-range directory: TIFFSetDirectory returns 0 silently without
  // invoking PrivateErrorHandler, so tiffinfo exits 0 with no output.
  const SubprocessResult bad_dir = RunTiffinfo({"-99", file.string()});
  EXPECT_EQ(bad_dir.term_signal, 0);
  EXPECT_EQ(bad_dir.exit_code, 0);
  EXPECT_THAT(bad_dir.stdout_output, IsEmpty());
  EXPECT_THAT(bad_dir.stderr_output, IsEmpty());

  // Select directory 5 by decimal and hex offset (-o).
  const SubprocessResult by_dec_off = RunTiffinfo(
      {"-o", absl::StrFormat("%llu", dir_offsets[5]), file.string()});
  EXPECT_EQ(by_dec_off.term_signal, 0);
  EXPECT_EQ(by_dec_off.exit_code, 0);
  EXPECT_THAT(by_dec_off.stderr_output, IsEmpty());
  EXPECT_THAT(by_dec_off.stdout_output,
              HasSubstr("Image Width: 7 Image Length: 1"));

  const SubprocessResult by_hex_off = RunTiffinfo(
      {"-o", absl::StrFormat("0x%llx", dir_offsets[5]), file.string()});
  EXPECT_EQ(by_hex_off.term_signal, 0);
  EXPECT_EQ(by_hex_off.exit_code, 0);
  EXPECT_THAT(by_hex_off.stderr_output, IsEmpty());
  EXPECT_THAT(by_hex_off.stdout_output,
              HasSubstr("Image Width: 7 Image Length: 1"));

  // Invalid IFD offset fails cleanly.
  const SubprocessResult bad_off = RunTiffinfo({"-o", "4", file.string()});
  EXPECT_EQ(bad_off.term_signal, 0);
  EXPECT_EQ(bad_off.exit_code, 1);
  EXPECT_THAT(bad_off.stderr_output, Not(IsEmpty()));
}

TEST(TiffinfoTest, FlagsColormapCurvesJpegStripsChopAndUnknownTagWarning) {
  TempDir dir("flags");

  // Palette image with ColorMap + -c.
  const std::filesystem::path palette_path = dir.child("palette.tif");
  {
    DiagnosticRecorder diag;
    UniqueTiff tif = OpenForWrite(palette_path, "wl", diag);
    ASSERT_NE(tif.get(), nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 4), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 2), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_PALETTE),
              1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 2), 1);
    std::array<uint16_t, 256> red{};
    std::array<uint16_t, 256> green{};
    std::array<uint16_t, 256> blue{};
    for (size_t i = 0; i < 256; ++i) {
      red[i] = static_cast<uint16_t>(i * 257);
      green[i] = static_cast<uint16_t>((255 - i) * 257);
      blue[i] = static_cast<uint16_t>((i / 2) * 257);
    }
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COLORMAP, red.data(),
                           green.data(), blue.data()),
              1);
    std::array<uint8_t, 4> row = {0, 1, 2, 3};
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), 1);
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 1, 0), 1);
    tif.reset();
    EXPECT_EQ(diag.error_count(), 0U);
  }
  const SubprocessResult cmap_res = RunTiffinfo({"-c", palette_path.native()});
  EXPECT_EQ(cmap_res.term_signal, 0);
  EXPECT_EQ(cmap_res.exit_code, 0);
  EXPECT_THAT(cmap_res.stdout_output, HasSubstr("Color Map:"));
  EXPECT_THAT(cmap_res.stderr_output, IsEmpty());

  // Grayscale image with TransferFunction + -c.
  const std::filesystem::path curve_path = dir.child("curve.tif");
  {
    DiagnosticRecorder diag;
    UniqueTiff tif = OpenForWrite(curve_path, "wl", diag);
    ASSERT_NE(tif.get(), nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 2), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 2), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 2), 1);
    std::array<uint16_t, 256> tf{};
    for (size_t i = 0; i < 256; ++i) {
      tf[i] = static_cast<uint16_t>(i * 256);
    }
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TRANSFERFUNCTION, tf.data()), 1);
    std::array<uint8_t, 2> row = {10, 20};
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), 1);
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 1, 0), 1);
    tif.reset();
    EXPECT_EQ(diag.error_count(), 0U);
  }
  const SubprocessResult curve_res = RunTiffinfo({"-c", curve_path.native()});
  EXPECT_EQ(curve_res.term_signal, 0);
  EXPECT_EQ(curve_res.exit_code, 0);
  EXPECT_THAT(curve_res.stdout_output, HasSubstr("Transfer Function:"));
  EXPECT_THAT(curve_res.stderr_output, IsEmpty());

  // JPEG image with -j and -s.
  if (TIFFIsCODECConfigured(COMPRESSION_JPEG) == 1) {
    const std::filesystem::path jpeg_path = dir.child("jpeg.tif");
    {
      DiagnosticRecorder diag;
      UniqueTiff tif = OpenForWrite(jpeg_path, "wl", diag);
      ASSERT_NE(tif.get(), nullptr);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 16), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 16), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 3), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
                1);
      ASSERT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 16), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGQUALITY, 90), 1);
      std::array<uint8_t, 16 * 3> row{};
      for (size_t i = 0; i < row.size(); ++i) {
        row[i] = static_cast<uint8_t>((i * 17) & 0xffU);
      }
      for (uint32_t r = 0; r < 16; ++r) {
        ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), r, 0), 1);
      }
      tif.reset();
      EXPECT_EQ(diag.error_count(), 0U);
    }
    const SubprocessResult jpeg_res =
        RunTiffinfo({"-j", "-s", "-D", jpeg_path.native()});
    EXPECT_EQ(jpeg_res.term_signal, 0);
    EXPECT_EQ(jpeg_res.exit_code, 0);
    EXPECT_THAT(jpeg_res.stdout_output, HasSubstr("JPEG Tables:"));
    EXPECT_THAT(jpeg_res.stdout_output, HasSubstr("1 Strips:"));
    EXPECT_THAT(jpeg_res.stderr_output, IsEmpty());
  }

  // Large uncompressed single-strip image: -z enables strip chopping ("rC").
  const std::filesystem::path chop_path = dir.child("chop.tif");
  WriteMinimalStripedGray8(chop_path, "wl", 64, 512, 512, FILLORDER_MSB2LSB,
                           COMPRESSION_NONE, 0x01);
  const SubprocessResult no_chop = RunTiffinfo({"-s", chop_path.native()});
  EXPECT_EQ(no_chop.exit_code, 0);
  EXPECT_THAT(no_chop.stdout_output, HasSubstr("1 Strips:"));
  EXPECT_THAT(no_chop.stderr_output, IsEmpty());

  const SubprocessResult with_chop =
      RunTiffinfo({"-z", "-s", chop_path.native()});
  EXPECT_EQ(with_chop.exit_code, 0);
  EXPECT_THAT(with_chop.stdout_output, HasSubstr("4 Strips:"));
  EXPECT_THAT(with_chop.stderr_output, IsEmpty());

  // Unknown tag warning with -W vs silent without -W.
  const std::filesystem::path unknown_tag_path = dir.child("unknown_tag.tif");
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  const std::array<uint8_t, 1> pixel = {0x42};
  const uint64_t strip_off = builder.AppendBlob(pixel);
  builder.AddIfd({
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, 1),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, 1),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(strip_off)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
      builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 1),
      builder.MakeLong(65000, 12345),
  });
  WriteBytesToFile(unknown_tag_path, builder.bytes());

  const SubprocessResult silent_unknown =
      RunTiffinfo({unknown_tag_path.native()});
  EXPECT_EQ(silent_unknown.term_signal, 0);
  EXPECT_EQ(silent_unknown.exit_code, 0);
  EXPECT_THAT(silent_unknown.stderr_output, IsEmpty());

  const SubprocessResult warn_unknown =
      RunTiffinfo({"-W", unknown_tag_path.native()});
  EXPECT_EQ(warn_unknown.term_signal, 0);
  EXPECT_EQ(warn_unknown.exit_code, 0);
  EXPECT_THAT(warn_unknown.stderr_output, HasSubstr("65000"));
}

TEST(TiffinfoTest, StripedContigAndSeparateReadAndDisplay) {
  TempDir dir("striped_data");

  // Contiguous strip with scanline=28 (>= 24 to cover ShowStrip line wrapping)
  // and 3 rows across 2 strips (rows_per_strip=2).
  const std::filesystem::path contig_path = dir.child("contig.tif");
  WriteMinimalStripedGray8(contig_path, "wl", 28, 3, 2, FILLORDER_MSB2LSB,
                           COMPRESSION_NONE, 0x10);

  const SubprocessResult contig_read_only =
      RunTiffinfo({"-D", contig_path.native()});
  EXPECT_EQ(contig_read_only.exit_code, 0);
  EXPECT_THAT(contig_read_only.stdout_output, Not(HasSubstr("Strip 0:")));
  EXPECT_THAT(contig_read_only.stderr_output, IsEmpty());

  const SubprocessResult contig_display =
      RunTiffinfo({"-d", "-f", "msb2lsb", contig_path.native()});
  EXPECT_EQ(contig_display.exit_code, 0);
  EXPECT_THAT(contig_display.stdout_output, HasSubstr("Strip 0:"));
  EXPECT_THAT(contig_display.stdout_output, HasSubstr("Strip 1:"));
  EXPECT_THAT(contig_display.stdout_output, HasSubstr(" 10 11 12"));
  EXPECT_THAT(contig_display.stderr_output, IsEmpty());

  // Separate planar configuration (3 samples, 26 bytes/row, 3 rows, 2
  // rows/strip).
  const std::filesystem::path sep_path = dir.child("separate.tif");
  {
    DiagnosticRecorder diag;
    UniqueTiff tif = OpenForWrite(sep_path, "wl", diag);
    ASSERT_NE(tif.get(), nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 26), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 3), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 3), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB), 1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE),
        1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 2), 1);
    std::array<uint8_t, 26> plane_row{};
    for (uint16_t s = 0; s < 3; ++s) {
      for (uint32_t r = 0; r < 3; ++r) {
        plane_row.fill(static_cast<uint8_t>(0xa0 + s * 0x10 + r));
        ASSERT_EQ(TIFFWriteScanline(tif.get(), plane_row.data(), r, s), 1);
      }
    }
    tif.reset();
    EXPECT_EQ(diag.error_count(), 0U);
  }

  const SubprocessResult sep_read_only = RunTiffinfo({"-D", sep_path.native()});
  EXPECT_EQ(sep_read_only.exit_code, 0);
  EXPECT_THAT(sep_read_only.stdout_output, Not(HasSubstr("Strip 0:")));
  EXPECT_THAT(sep_read_only.stderr_output, IsEmpty());

  const SubprocessResult sep_display = RunTiffinfo({"-d", sep_path.native()});
  EXPECT_EQ(sep_display.exit_code, 0);
  EXPECT_THAT(sep_display.stdout_output, HasSubstr("Strip 0:"));
  EXPECT_THAT(sep_display.stdout_output, HasSubstr("Strip 5:"));
  EXPECT_THAT(sep_display.stdout_output, HasSubstr(" a0 a0 a0"));
  EXPECT_THAT(sep_display.stdout_output, HasSubstr(" c2 c2 c2"));
  EXPECT_THAT(sep_display.stderr_output, IsEmpty());
}

TEST(TiffinfoTest, TiledContigAndSeparateReadAndDisplay) {
  TempDir dir("tiled_data");

  // Contiguous tiled image: 32x32 with 32x16 tiles (rowsize=32 >= 24).
  const std::filesystem::path contig_tile = dir.child("contig_tile.tif");
  {
    DiagnosticRecorder diag;
    UniqueTiff tif = OpenForWrite(contig_tile, "wl", diag);
    ASSERT_NE(tif.get(), nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 32), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 32), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, 32), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, 16), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
    std::vector<uint8_t> tile0(32 * 16, 0x41);
    std::vector<uint8_t> tile1(32 * 16, 0x52);
    ASSERT_NE(TIFFWriteTile(tif.get(), tile0.data(), 0, 0, 0, 0),
              static_cast<tmsize_t>(-1));
    ASSERT_NE(TIFFWriteTile(tif.get(), tile1.data(), 0, 16, 0, 0),
              static_cast<tmsize_t>(-1));
    tif.reset();
    EXPECT_EQ(diag.error_count(), 0U);
  }

  const SubprocessResult contig_d =
      RunTiffinfo({"-d", "-s", contig_tile.native()});
  EXPECT_EQ(contig_d.exit_code, 0);
  EXPECT_THAT(contig_d.stdout_output, HasSubstr("Tile (0,0):"));
  EXPECT_THAT(contig_d.stdout_output, HasSubstr("Tile (16,0):"));
  EXPECT_THAT(contig_d.stdout_output, HasSubstr(" 41 41 41"));
  EXPECT_THAT(contig_d.stdout_output, HasSubstr(" 52 52 52"));
  EXPECT_THAT(contig_d.stdout_output, HasSubstr("2 Tiles:"));
  EXPECT_THAT(contig_d.stderr_output, IsEmpty());

  const SubprocessResult contig_D = RunTiffinfo({"-D", contig_tile.native()});
  EXPECT_EQ(contig_D.exit_code, 0);
  EXPECT_THAT(contig_D.stdout_output, Not(HasSubstr("Tile (0,0):")));
  EXPECT_THAT(contig_D.stderr_output, IsEmpty());

  // Separate tiled image: 32x16, 3 samples, 32x16 tiles.
  const std::filesystem::path sep_tile = dir.child("sep_tile.tif");
  {
    DiagnosticRecorder diag;
    UniqueTiff tif = OpenForWrite(sep_tile, "wl", diag);
    ASSERT_NE(tif.get(), nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 32), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 16), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, 32), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, 16), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 3), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB), 1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE),
        1);
    for (uint16_t s = 0; s < 3; ++s) {
      std::vector<uint8_t> tile(32 * 16, static_cast<uint8_t>(0x70 + s));
      ASSERT_NE(TIFFWriteTile(tif.get(), tile.data(), 0, 0, 0, s),
                static_cast<tmsize_t>(-1));
    }
    tif.reset();
    EXPECT_EQ(diag.error_count(), 0U);
  }

  const SubprocessResult sep_d = RunTiffinfo({"-d", sep_tile.native()});
  EXPECT_EQ(sep_d.exit_code, 0);
  EXPECT_THAT(sep_d.stdout_output, HasSubstr("Tile (0,0,0):"));
  EXPECT_THAT(sep_d.stdout_output, HasSubstr("Tile (0,0,1):"));
  EXPECT_THAT(sep_d.stdout_output, HasSubstr("Tile (0,0,2):"));
  EXPECT_THAT(sep_d.stdout_output, HasSubstr(" 70 70 70"));
  EXPECT_THAT(sep_d.stdout_output, HasSubstr(" 72 72 72"));
  EXPECT_THAT(sep_d.stderr_output, IsEmpty());
}

TEST(TiffinfoTest, RawDataStripedAndTiledBitOrderAndWords) {
  TempDir dir("raw_data");

  // Striped file with 2 strips of increasing sizes (16 bytes then 34 bytes) so
  // TIFFReadRawDataStriped exercises realloc, ShowRawBytes wrap (>= 24), and
  // ShowRawWords wrap (>= 15 words).
  const std::filesystem::path raw_strip_path = dir.child("raw_strip.tif");
  RawTiffBuilder strip_builder(/*big_endian=*/false, /*big_tiff=*/false);
  std::vector<uint8_t> s0_bytes(16, 0x33);
  std::vector<uint8_t> s1_bytes(34);
  for (size_t i = 0; i < s1_bytes.size(); ++i) {
    s1_bytes[i] = static_cast<uint8_t>(i);
  }
  const uint64_t s0_off = strip_builder.AppendBlob(s0_bytes);
  const uint64_t s1_off = strip_builder.AppendBlob(s1_bytes);
  strip_builder.AddIfd({
      strip_builder.MakeLong(TIFFTAG_IMAGEWIDTH, 34),
      strip_builder.MakeLong(TIFFTAG_IMAGELENGTH, 2),
      strip_builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      strip_builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
      strip_builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      strip_builder.MakeShort(TIFFTAG_FILLORDER, FILLORDER_MSB2LSB),
      strip_builder.MakeLongArray(
          TIFFTAG_STRIPOFFSETS,
          std::array<uint32_t, 2>{static_cast<uint32_t>(s0_off),
                                  static_cast<uint32_t>(s1_off)}),
      strip_builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      strip_builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
      strip_builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS,
                                  std::array<uint32_t, 2>{16, 34}),
  });
  WriteBytesToFile(raw_strip_path, strip_builder.bytes());

  const SubprocessResult raw_D =
      RunTiffinfo({"-r", "-D", raw_strip_path.native()});
  EXPECT_EQ(raw_D.exit_code, 0);
  EXPECT_THAT(raw_D.stdout_output, Not(HasSubstr("Strip 0:")));
  EXPECT_THAT(raw_D.stderr_output, IsEmpty());

  const SubprocessResult raw_d =
      RunTiffinfo({"-r", "-d", raw_strip_path.native()});
  EXPECT_EQ(raw_d.exit_code, 0);
  EXPECT_THAT(raw_d.stdout_output, HasSubstr("Strip 0:\n"));
  EXPECT_THAT(raw_d.stdout_output, HasSubstr("Strip 1:\n"));
  EXPECT_THAT(raw_d.stdout_output, HasSubstr(" 33 33 33"));
  EXPECT_THAT(raw_d.stdout_output, Not(HasSubstr("bit reversed")));
  EXPECT_THAT(raw_d.stderr_output, IsEmpty());

  const SubprocessResult raw_d_words =
      RunTiffinfo({"-r", "-d", "-w", raw_strip_path.native()});
  EXPECT_EQ(raw_d_words.exit_code, 0);
  EXPECT_THAT(raw_d_words.stdout_output, HasSubstr(" 3333 3333"));
  EXPECT_THAT(raw_d_words.stderr_output, IsEmpty());

  const SubprocessResult raw_d_same_order =
      RunTiffinfo({"-r", "-d", "-f", "msb2lsb", raw_strip_path.native()});
  EXPECT_EQ(raw_d_same_order.exit_code, 0);
  EXPECT_THAT(raw_d_same_order.stdout_output, Not(HasSubstr("bit reversed")));
  EXPECT_THAT(raw_d_same_order.stderr_output, IsEmpty());

  // 0x33 bit-reversed is 0xcc.
  const SubprocessResult raw_d_bitrev =
      RunTiffinfo({"-r", "-d", "-f", "lsb2msb", raw_strip_path.native()});
  EXPECT_EQ(raw_d_bitrev.exit_code, 0);
  EXPECT_THAT(raw_d_bitrev.stdout_output, HasSubstr("Strip 0: (bit reversed)"));
  EXPECT_THAT(raw_d_bitrev.stdout_output, HasSubstr(" cc cc cc"));
  EXPECT_THAT(raw_d_bitrev.stderr_output, IsEmpty());

  // Tiled raw read with increasing tile byte counts (16 then 32 bytes).
  const std::filesystem::path raw_tile_path = dir.child("raw_tile.tif");
  RawTiffBuilder tile_builder(/*big_endian=*/false, /*big_tiff=*/false);
  std::vector<uint8_t> t0_bytes(16, 0x0f);
  std::vector<uint8_t> t1_bytes(32);
  for (size_t i = 0; i < t1_bytes.size(); ++i) {
    t1_bytes[i] = static_cast<uint8_t>(0x80 + i);
  }
  const uint64_t t0_off = tile_builder.AppendBlob(t0_bytes);
  const uint64_t t1_off = tile_builder.AppendBlob(t1_bytes);
  tile_builder.AddIfd({
      tile_builder.MakeLong(TIFFTAG_IMAGEWIDTH, 32),
      tile_builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
      tile_builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      tile_builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
      tile_builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      tile_builder.MakeShort(TIFFTAG_FILLORDER, FILLORDER_LSB2MSB),
      tile_builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      tile_builder.MakeLong(TIFFTAG_TILEWIDTH, 16),
      tile_builder.MakeLong(TIFFTAG_TILELENGTH, 16),
      tile_builder.MakeLongArray(
          TIFFTAG_TILEOFFSETS,
          std::array<uint32_t, 2>{static_cast<uint32_t>(t0_off),
                                  static_cast<uint32_t>(t1_off)}),
      tile_builder.MakeLongArray(TIFFTAG_TILEBYTECOUNTS,
                                 std::array<uint32_t, 2>{16, 32}),
  });
  WriteBytesToFile(raw_tile_path, tile_builder.bytes());

  const SubprocessResult raw_tile_D =
      RunTiffinfo({"-r", "-D", raw_tile_path.native()});
  EXPECT_EQ(raw_tile_D.exit_code, 0);
  EXPECT_THAT(raw_tile_D.stderr_output, IsEmpty());

  const SubprocessResult raw_tile_d =
      RunTiffinfo({"-r", "-d", raw_tile_path.native()});
  EXPECT_EQ(raw_tile_d.exit_code, 0);
  EXPECT_THAT(raw_tile_d.stdout_output, HasSubstr("Tile 0:\n"));
  EXPECT_THAT(raw_tile_d.stdout_output, HasSubstr("Tile 1:\n"));
  EXPECT_THAT(raw_tile_d.stdout_output, HasSubstr(" 0f 0f 0f"));
  EXPECT_THAT(raw_tile_d.stderr_output, IsEmpty());

  const SubprocessResult raw_tile_words =
      RunTiffinfo({"-r", "-d", "-w", raw_tile_path.native()});
  EXPECT_EQ(raw_tile_words.exit_code, 0);
  EXPECT_THAT(raw_tile_words.stdout_output, HasSubstr(" 0f0f 0f0f"));
  EXPECT_THAT(raw_tile_words.stderr_output, IsEmpty());

  // 0x0f bit-reversed is 0xf0.
  const SubprocessResult raw_tile_bitrev =
      RunTiffinfo({"-r", "-d", "-f", "msb2lsb", raw_tile_path.native()});
  EXPECT_EQ(raw_tile_bitrev.exit_code, 0);
  EXPECT_THAT(raw_tile_bitrev.stdout_output,
              HasSubstr("Tile 0: (bit reversed)"));
  EXPECT_THAT(raw_tile_bitrev.stdout_output, HasSubstr(" f0 f0 f0"));
  EXPECT_THAT(raw_tile_bitrev.stderr_output, IsEmpty());
}

TEST(TiffinfoTest, BitDepthsAndSampleFormats) {
  TempDir dir("bit_depths");
  struct SampleCase {
    const char* name;
    uint16_t bps;
    uint16_t format;
    const char* expected_format_str;
    std::vector<uint8_t> row_bytes;
  };

  std::vector<uint8_t> float_row(4 * sizeof(float));
  const float fvals[4] = {0.0F, 1.0F, -2.5F, 3.25F};
  std::memcpy(float_row.data(), fvals, sizeof(fvals));

  const std::vector<SampleCase> cases = {
      {.name = "1bit.tif",
       .bps = 1,
       .format = SAMPLEFORMAT_UINT,
       .expected_format_str = "unsigned integer",
       .row_bytes = {0xa5}},
      {.name = "4bit.tif",
       .bps = 4,
       .format = SAMPLEFORMAT_UINT,
       .expected_format_str = "unsigned integer",
       .row_bytes = {0x12, 0x34}},
      {.name = "8bit.tif",
       .bps = 8,
       .format = SAMPLEFORMAT_UINT,
       .expected_format_str = "unsigned integer",
       .row_bytes = {0x11, 0x22, 0x33, 0x44}},
      {.name = "16bit_int.tif",
       .bps = 16,
       .format = SAMPLEFORMAT_INT,
       .expected_format_str = "signed integer",
       .row_bytes = {0x01, 0x00, 0xff, 0xff, 0x02, 0x00, 0xfe, 0xff}},
      {.name = "32bit_uint.tif",
       .bps = 32,
       .format = SAMPLEFORMAT_UINT,
       .expected_format_str = "unsigned integer",
       .row_bytes = {1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0}},
      {.name = "32bit_float.tif",
       .bps = 32,
       .format = SAMPLEFORMAT_IEEEFP,
       .expected_format_str = "IEEE floating point",
       .row_bytes = float_row},
  };

  for (const SampleCase& tc : cases) {
    SCOPED_TRACE(tc.name);
    const std::filesystem::path p = dir.child(tc.name);
    DiagnosticRecorder diag;
    UniqueTiff tif = OpenForWrite(p, "wl", diag);
    ASSERT_NE(tif.get(), nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 4), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 1), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, tc.bps), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLEFORMAT, tc.format), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 1), 1);
    std::vector<uint8_t> mutable_row = tc.row_bytes;
    ASSERT_EQ(TIFFWriteScanline(tif.get(), mutable_row.data(), 0, 0), 1);
    tif.reset();
    EXPECT_EQ(diag.error_count(), 0U);

    const SubprocessResult res = RunTiffinfo({"-d", p.native()});
    EXPECT_EQ(res.term_signal, 0);
    EXPECT_EQ(res.exit_code, 0);
    EXPECT_THAT(res.stdout_output,
                HasSubstr(absl::StrFormat("Bits/Sample: %u", tc.bps)));
    EXPECT_THAT(res.stdout_output, HasSubstr(tc.expected_format_str));
    EXPECT_THAT(res.stdout_output, HasSubstr("Strip 0:"));
    EXPECT_THAT(res.stderr_output, IsEmpty());
  }
}

TEST(TiffinfoTest, SubIfdsExifAndGpsDirectories) {
  TempFile file("subifd_exif_gps.tif");
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);

  const std::array<uint8_t, 4> pixel = {0x11, 0x22, 0x33, 0x44};
  const uint64_t main_strip_off = builder.AppendBlob(pixel);
  const uint64_t sub_strip_off = builder.AppendBlob(pixel);

  const uint64_t exif_off = builder.AddIfd(
      {
          builder.MakeRational(EXIFTAG_EXPOSURETIME, 1, 100),
          builder.MakeLong(EXIFTAG_ISOSPEED, 400),
      },
      /*explicit_next_ifd_offset=*/std::nullopt, /*sort_by_tag=*/true,
      /*link_from_previous=*/false);

  const uint64_t gps_off = builder.AddIfd(
      {
          builder.MakeAscii(GPSTAG_LATITUDEREF, "N"),
          builder.MakeRational(GPSTAG_DOP, 5, 2),
      },
      /*explicit_next_ifd_offset=*/std::nullopt, /*sort_by_tag=*/true,
      /*link_from_previous=*/false);

  auto make_sub_entries = [&](uint32_t w) {
    return std::vector<RawIfdEntry>{
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, w),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 1),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                         static_cast<uint32_t>(sub_strip_off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS, w),
    };
  };

  // Chain 0: sub0a -> sub0b. Chain 1: sub1.
  const uint64_t sub0b_off = builder.AddIfd(
      make_sub_entries(2), /*explicit_next_ifd_offset=*/std::nullopt,
      /*sort_by_tag=*/true, /*link_from_previous=*/false);
  const uint64_t sub0a_off = builder.AddIfd(
      make_sub_entries(3), /*explicit_next_ifd_offset=*/sub0b_off,
      /*sort_by_tag=*/true, /*link_from_previous=*/false);
  const uint64_t sub1_off = builder.AddIfd(
      make_sub_entries(4), /*explicit_next_ifd_offset=*/std::nullopt,
      /*sort_by_tag=*/true, /*link_from_previous=*/false);

  const std::array<uint32_t, 2> subifd_ptrs = {static_cast<uint32_t>(sub0a_off),
                                               static_cast<uint32_t>(sub1_off)};
  const uint64_t main_off = builder.AddIfd(
      {
          builder.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
          builder.MakeLong(TIFFTAG_IMAGELENGTH, 1),
          builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
          builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
          builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
          builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                           static_cast<uint32_t>(main_strip_off)),
          builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
          builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
          builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 4),
          builder.MakeLongArray(TIFFTAG_SUBIFD, subifd_ptrs, TIFF_IFD),
          builder.MakeLong(TIFFTAG_EXIFIFD, static_cast<uint32_t>(exif_off),
                           TIFF_IFD),
          builder.MakeLong(TIFFTAG_GPSIFD, static_cast<uint32_t>(gps_off),
                           TIFF_IFD),
      },
      /*explicit_next_ifd_offset=*/std::nullopt, /*sort_by_tag=*/true,
      /*link_from_previous=*/false);
  builder.PatchHeaderFirstIfdOffset(main_off);
  WriteBytesToFile(file.path(), builder.bytes());

  const SubprocessResult res = RunTiffinfo({"-d", file.string()});
  EXPECT_EQ(res.term_signal, 0);
  EXPECT_EQ(res.exit_code, 0);
  EXPECT_THAT(res.stderr_output, IsEmpty());
  EXPECT_THAT(res.stdout_output, HasSubstr("=== TIFF directory 0 ==="));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("--- EXIF directory within directory 0"));
  EXPECT_THAT(res.stdout_output, HasSubstr("ISOSpeed: 400"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("--- GPS directory within directory 0"));
  EXPECT_THAT(res.stdout_output, HasSubstr("LatitudeRef: N"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("--- SubIFD image descriptor tag within TIFF directory "
                        "0 with array of 2 SubIFD chains ---"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("--- SubIFD 0 of chain 0 at offset"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("--- SubIFD 1 of chain 0 at offset"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("--- SubIFD 0 of chain 1 at offset"));
}

TEST(TiffinfoTest, MemoryLimitsAcrossAllReadPaths) {
  TempDir dir("mem_limits");

  // Disable limit (-M 0) on normal file.
  const std::filesystem::path small_file = dir.child("small.tif");
  WriteMinimalStripedGray8(small_file, "wl", 2, 2, 2, FILLORDER_MSB2LSB,
                           COMPRESSION_NONE, 0x05);
  const SubprocessResult unlimited =
      RunTiffinfo({"-M", "0", "-D", small_file.native()});
  EXPECT_EQ(unlimited.term_signal, 0);
  EXPECT_EQ(unlimited.exit_code, 0);
  EXPECT_THAT(unlimited.stderr_output, IsEmpty());

  // Helper to write a small synthetic TIFF claiming > 1 MiB strip/tile size.
  auto write_huge_strip_meta = [&](const std::filesystem::path& p,
                                   uint16_t planar, uint16_t samples,
                                   uint32_t bytecount) {
    RawTiffBuilder b(/*big_endian=*/false, /*big_tiff=*/false);
    const std::array<uint8_t, 4> stub = {0, 0, 0, 0};
    const uint64_t off = b.AppendBlob(stub);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 2000000),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 1),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC,
                    samples == 1 ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB),
        b.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, samples),
        b.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        b.MakeLong(TIFFTAG_STRIPBYTECOUNTS, bytecount),
        b.MakeShort(TIFFTAG_PLANARCONFIG, planar),
    });
    WriteBytesToFile(p, b.bytes());
  };

  auto write_huge_tile_meta = [&](const std::filesystem::path& p,
                                  uint16_t planar, uint16_t samples,
                                  uint32_t bytecount) {
    RawTiffBuilder b(/*big_endian=*/false, /*big_tiff=*/false);
    const std::array<uint8_t, 4> stub = {0, 0, 0, 0};
    const uint64_t off = b.AppendBlob(stub);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 1024),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 2048),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC,
                    samples == 1 ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, samples),
        b.MakeShort(TIFFTAG_PLANARCONFIG, planar),
        b.MakeLong(TIFFTAG_TILEWIDTH, 1024),
        b.MakeLong(TIFFTAG_TILELENGTH, 2048),
        b.MakeLong(TIFFTAG_TILEOFFSETS, static_cast<uint32_t>(off)),
        b.MakeLong(TIFFTAG_TILEBYTECOUNTS, bytecount),
    });
    WriteBytesToFile(p, b.bytes());
  };

  const std::filesystem::path contig_strip = dir.child("huge_contig_strip.tif");
  write_huge_strip_meta(contig_strip, PLANARCONFIG_CONTIG, 1, 2000000);
  const SubprocessResult res_cs =
      RunTiffinfo({"-M", "1", "-D", contig_strip.native()});
  EXPECT_EQ(res_cs.exit_code, 0);
  EXPECT_EQ(res_cs.term_signal, 0);
  EXPECT_THAT(res_cs.stderr_output, HasSubstr("over memory limit"));

  const std::filesystem::path raw_strip_res_path =
      dir.child("raw_strip_lim.tif");
  write_huge_strip_meta(raw_strip_res_path, PLANARCONFIG_CONTIG, 1, 2000000);
  const SubprocessResult res_rs =
      RunTiffinfo({"-M", "1", "-r", "-D", raw_strip_res_path.native()});
  EXPECT_EQ(res_rs.exit_code, 0);
  EXPECT_EQ(res_rs.term_signal, 0);
  EXPECT_THAT(res_rs.stderr_output, HasSubstr("over memory limit"));

  // Separate strip needs >1 MiB per single plane strip.
  const std::filesystem::path sep_strip = dir.child("huge_sep_strip.tif");
  {
    RawTiffBuilder b(/*big_endian=*/false, /*big_tiff=*/false);
    const std::array<uint8_t, 4> stub = {0, 0, 0, 0};
    const uint64_t off = b.AppendBlob(stub);
    const uint32_t uoff = static_cast<uint32_t>(off);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 2000000),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 1),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
        b.MakeLongArray(TIFFTAG_STRIPOFFSETS,
                        std::array<uint32_t, 3>{uoff, uoff, uoff}),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        b.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        b.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS,
                        std::array<uint32_t, 3>{2000000, 2000000, 2000000}),
        b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE),
    });
    WriteBytesToFile(sep_strip, b.bytes());
  }
  const SubprocessResult res_ss =
      RunTiffinfo({"-M", "1", "-D", sep_strip.native()});
  EXPECT_EQ(res_ss.exit_code, 0);
  EXPECT_EQ(res_ss.term_signal, 0);
  EXPECT_THAT(res_ss.stderr_output, HasSubstr("over memory limit"));

  const std::filesystem::path contig_tile = dir.child("huge_contig_tile.tif");
  write_huge_tile_meta(contig_tile, PLANARCONFIG_CONTIG, 1, 2097152);
  const SubprocessResult res_ct =
      RunTiffinfo({"-M", "1", "-D", contig_tile.native()});
  EXPECT_EQ(res_ct.exit_code, 0);
  EXPECT_EQ(res_ct.term_signal, 0);
  EXPECT_THAT(res_ct.stderr_output, HasSubstr("over memory limit"));

  const SubprocessResult res_rt =
      RunTiffinfo({"-M", "1", "-r", "-D", contig_tile.native()});
  EXPECT_EQ(res_rt.exit_code, 0);
  EXPECT_EQ(res_rt.term_signal, 0);
  EXPECT_THAT(res_rt.stderr_output, HasSubstr("over memory limit"));

  const std::filesystem::path sep_tile = dir.child("huge_sep_tile.tif");
  {
    RawTiffBuilder b(/*big_endian=*/false, /*big_tiff=*/false);
    const std::array<uint8_t, 4> stub = {0, 0, 0, 0};
    const uint64_t off = b.AppendBlob(stub);
    const uint32_t uoff = static_cast<uint32_t>(off);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 1024),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 2048),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE),
        b.MakeLong(TIFFTAG_TILEWIDTH, 1024),
        b.MakeLong(TIFFTAG_TILELENGTH, 2048),
        b.MakeLongArray(TIFFTAG_TILEOFFSETS,
                        std::array<uint32_t, 3>{uoff, uoff, uoff}),
        b.MakeLongArray(TIFFTAG_TILEBYTECOUNTS,
                        std::array<uint32_t, 3>{2097152, 2097152, 2097152}),
    });
    WriteBytesToFile(sep_tile, b.bytes());
  }
  const SubprocessResult res_st =
      RunTiffinfo({"-M", "1", "-D", sep_tile.native()});
  EXPECT_EQ(res_st.exit_code, 0);
  EXPECT_EQ(res_st.term_signal, 0);
  EXPECT_THAT(res_st.stderr_output, HasSubstr("over memory limit"));
}

TEST(TiffinfoTest, SizeOverflowAndYcbcrSubsampledTileBounds) {
  TempDir dir("overflow_bounds");

  // Contig YCbCr 2x2 subsampled uncompressed tile: TIFFTileSize < th * rowsize,
  // hitting "Cannot display data: th * rowsize > tilesize".
  const std::filesystem::path ycbcr_tile = dir.child("ycbcr_tile.tif");
  {
    RawTiffBuilder b(/*big_endian=*/false, /*big_tiff=*/false);
    std::vector<uint8_t> tile_data(16 * 16 * 3 / 2, 0x80);
    const uint64_t off = b.AppendBlob(tile_data);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        b.MakeLong(TIFFTAG_TILEWIDTH, 16),
        b.MakeLong(TIFFTAG_TILELENGTH, 16),
        b.MakeLong(TIFFTAG_TILEOFFSETS, static_cast<uint32_t>(off)),
        b.MakeLong(TIFFTAG_TILEBYTECOUNTS,
                   static_cast<uint32_t>(tile_data.size())),
        b.MakeShortArray(TIFFTAG_YCBCRSUBSAMPLING,
                         std::array<uint16_t, 2>{2, 2}),
    });
    WriteBytesToFile(ycbcr_tile, b.bytes());
  }
  const SubprocessResult ycbcr_res = RunTiffinfo({"-D", ycbcr_tile.native()});
  EXPECT_EQ(ycbcr_res.term_signal, 0);
  EXPECT_EQ(ycbcr_res.exit_code, 0);
  EXPECT_THAT(ycbcr_res.stderr_output,
              HasSubstr("Cannot display data: th * rowsize > tilesize"));

  // BigTIFF raw strip and tile byte counts exceeding INT64_MAX cause
  // _TIFFCastUInt64ToSSize to return 0 while bytecount != 0. Use 2 strips with
  // PackBits so TIFFReadDirectory does not override StripByteCounts[0].
  const std::filesystem::path big_raw_strip = dir.child("big_raw_strip.tif");
  {
    RawTiffBuilder b(/*big_endian=*/false, /*big_tiff=*/true);
    const std::array<uint8_t, 4> stub = {1, 2, 3, 4};
    const uint64_t off = b.AppendBlob(stub);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 2),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        b.MakeLong8Array(TIFFTAG_STRIPOFFSETS,
                         std::array<uint64_t, 2>{off, off}, TIFF_LONG8),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        b.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        b.MakeLong8Array(TIFFTAG_STRIPBYTECOUNTS,
                         std::array<uint64_t, 2>{(1ULL << 63) + 1ULL, 4ULL},
                         TIFF_LONG8),
    });
    WriteBytesToFile(big_raw_strip, b.bytes());
  }
  const SubprocessResult big_strip_res =
      RunTiffinfo({"-r", "-D", big_raw_strip.native()});
  EXPECT_EQ(big_strip_res.term_signal, 0);
  EXPECT_EQ(big_strip_res.exit_code, 1);
  EXPECT_THAT(big_strip_res.stderr_output, HasSubstr("Integer overflow"));

  const std::filesystem::path big_raw_tile = dir.child("big_raw_tile.tif");
  {
    RawTiffBuilder b(/*big_endian=*/false, /*big_tiff=*/true);
    const std::array<uint8_t, 4> stub = {1, 2, 3, 4};
    const uint64_t off = b.AppendBlob(stub);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        b.MakeLong(TIFFTAG_TILEWIDTH, 16),
        b.MakeLong(TIFFTAG_TILELENGTH, 16),
        b.MakeLong8(TIFFTAG_TILEOFFSETS, off, TIFF_LONG8),
        b.MakeLong8(TIFFTAG_TILEBYTECOUNTS, (1ULL << 63) + 1ULL, TIFF_LONG8),
    });
    WriteBytesToFile(big_raw_tile, b.bytes());
  }
  const SubprocessResult big_tile_res =
      RunTiffinfo({"-r", "-D", big_raw_tile.native()});
  EXPECT_EQ(big_tile_res.term_signal, 0);
  EXPECT_EQ(big_tile_res.exit_code, 1);
  EXPECT_THAT(big_tile_res.stderr_output, HasSubstr("Integer overflow"));
}

TEST(TiffinfoTest, ReadErrorsStopVsIgnoreAcrossStripAndTilePaths) {
  TempDir dir("read_errors");

  // 2 strips where strip 0 offset points past EOF and strip 1 is valid.
  const std::filesystem::path err_contig_strip =
      dir.child("err_contig_strip.tif");
  {
    RawTiffBuilder b(/*big_endian=*/false, /*big_tiff=*/false);
    const std::array<uint8_t, 4> valid_strip = {0x55, 0x66, 0x77, 0x88};
    const uint64_t valid_off = b.AppendBlob(valid_strip);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 2),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        b.MakeLongArray(TIFFTAG_STRIPOFFSETS,
                        std::array<uint32_t, 2>{
                            0x7fffffffU, static_cast<uint32_t>(valid_off)}),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        b.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        b.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, std::array<uint32_t, 2>{4, 4}),
    });
    WriteBytesToFile(err_contig_strip, b.bytes());
  }

  for (const bool raw : {false, true}) {
    std::vector<std::string> stop_args = {"-d"};
    std::vector<std::string> cont_args = {"-i", "-d"};
    if (raw) {
      stop_args.insert(stop_args.begin(), "-r");
      cont_args.insert(cont_args.begin(), "-r");
    }
    stop_args.push_back(err_contig_strip.native());
    cont_args.push_back(err_contig_strip.native());

    const SubprocessResult stopped = RunTiffinfo(stop_args);
    EXPECT_EQ(stopped.term_signal, 0);
    EXPECT_EQ(stopped.exit_code, 1);
    EXPECT_THAT(stopped.stdout_output, Not(HasSubstr("Strip 1:")));
    EXPECT_THAT(stopped.stderr_output, Not(IsEmpty()));

    const SubprocessResult continued = RunTiffinfo(cont_args);
    EXPECT_EQ(continued.term_signal, 0);
    EXPECT_EQ(continued.exit_code, 1);
    EXPECT_THAT(continued.stdout_output, HasSubstr("Strip 1:"));
    EXPECT_THAT(continued.stderr_output, Not(IsEmpty()));
  }

  // Separate strip error: 2 planes, 1 row, plane 0 past EOF, plane 1 valid.
  const std::filesystem::path err_sep_strip = dir.child("err_sep_strip.tif");
  {
    RawTiffBuilder b(/*big_endian=*/false, /*big_tiff=*/false);
    const std::array<uint8_t, 4> valid_strip = {0x12, 0x34, 0x56, 0x78};
    const uint64_t valid_off = b.AppendBlob(valid_strip);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 1),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE),
        b.MakeLongArray(TIFFTAG_STRIPOFFSETS,
                        std::array<uint32_t, 2>{
                            0x7fffffffU, static_cast<uint32_t>(valid_off)}),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 2),
        b.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        b.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, std::array<uint32_t, 2>{4, 4}),
    });
    WriteBytesToFile(err_sep_strip, b.bytes());
  }

  const SubprocessResult sep_stop = RunTiffinfo({"-d", err_sep_strip.native()});
  EXPECT_EQ(sep_stop.term_signal, 0);
  EXPECT_EQ(sep_stop.exit_code, 1);
  EXPECT_THAT(sep_stop.stdout_output, Not(HasSubstr("Strip 1:")));

  const SubprocessResult sep_cont =
      RunTiffinfo({"-i", "-d", err_sep_strip.native()});
  EXPECT_EQ(sep_cont.term_signal, 0);
  EXPECT_EQ(sep_cont.exit_code, 1);
  EXPECT_THAT(sep_cont.stdout_output, HasSubstr("Strip 1:"));

  // Contiguous tiled error (raw): tile 0 past EOF, tile 1 valid.
  const std::filesystem::path err_contig_tile_raw =
      dir.child("err_contig_tile_raw.tif");
  {
    RawTiffBuilder b(/*big_endian=*/false, /*big_tiff=*/false);
    std::vector<uint8_t> valid_tile(16 * 16, 0x99);
    const uint64_t valid_off = b.AppendBlob(valid_tile);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 32),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        b.MakeLong(TIFFTAG_TILEWIDTH, 16),
        b.MakeLong(TIFFTAG_TILELENGTH, 16),
        b.MakeLongArray(TIFFTAG_TILEOFFSETS,
                        std::array<uint32_t, 2>{
                            0x7fffffffU, static_cast<uint32_t>(valid_off)}),
        b.MakeLongArray(TIFFTAG_TILEBYTECOUNTS,
                        std::array<uint32_t, 2>{256, 256}),
    });
    WriteBytesToFile(err_contig_tile_raw, b.bytes());
  }

  {
    const SubprocessResult stopped =
        RunTiffinfo({"-r", "-d", err_contig_tile_raw.native()});
    EXPECT_EQ(stopped.term_signal, 0);
    EXPECT_EQ(stopped.exit_code, 1);
    EXPECT_THAT(stopped.stdout_output, Not(HasSubstr("Tile 1:")));
    EXPECT_THAT(stopped.stderr_output, Not(IsEmpty()));

    const SubprocessResult continued =
        RunTiffinfo({"-r", "-i", "-d", err_contig_tile_raw.native()});
    EXPECT_EQ(continued.term_signal, 0);
    EXPECT_EQ(continued.exit_code, 1);
    EXPECT_THAT(continued.stdout_output, HasSubstr("Tile 1:"));
    EXPECT_THAT(continued.stderr_output, Not(IsEmpty()));
  }

  // Contiguous tiled error (decoded): PackBits tile 0 truncated, tile 1 valid
  // (two 128-byte runs of 0x99).
  const std::filesystem::path err_contig_tile_dec =
      dir.child("err_contig_tile_dec.tif");
  {
    RawTiffBuilder b(/*big_endian=*/false, /*big_tiff=*/false);
    const std::array<uint8_t, 2> bad_packbits = {0x00, 0xaa};
    const std::array<uint8_t, 4> good_packbits = {0x81, 0x99, 0x81, 0x99};
    const uint64_t bad_off = b.AppendBlob(bad_packbits);
    const uint64_t good_off = b.AppendBlob(good_packbits);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 32),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        b.MakeLong(TIFFTAG_TILEWIDTH, 16),
        b.MakeLong(TIFFTAG_TILELENGTH, 16),
        b.MakeLongArray(
            TIFFTAG_TILEOFFSETS,
            std::array<uint32_t, 2>{static_cast<uint32_t>(bad_off),
                                    static_cast<uint32_t>(good_off)}),
        b.MakeLongArray(TIFFTAG_TILEBYTECOUNTS, std::array<uint32_t, 2>{2, 4}),
    });
    WriteBytesToFile(err_contig_tile_dec, b.bytes());
  }

  {
    const SubprocessResult stopped =
        RunTiffinfo({"-d", err_contig_tile_dec.native()});
    EXPECT_EQ(stopped.term_signal, 0);
    EXPECT_EQ(stopped.exit_code, 1);
    EXPECT_THAT(stopped.stdout_output, Not(HasSubstr("Tile (0,16):")));
    EXPECT_THAT(stopped.stderr_output, Not(IsEmpty()));

    const SubprocessResult continued =
        RunTiffinfo({"-i", "-d", err_contig_tile_dec.native()});
    EXPECT_EQ(continued.term_signal, 0);
    EXPECT_EQ(continued.exit_code, 1);
    EXPECT_THAT(continued.stdout_output, HasSubstr("Tile (0,16):"));
    EXPECT_THAT(continued.stderr_output, Not(IsEmpty()));
  }

  // Separate tiled error (decoded): PackBits plane 0 truncated, plane 1 valid.
  const std::filesystem::path err_sep_tile = dir.child("err_sep_tile.tif");
  {
    RawTiffBuilder b(/*big_endian=*/false, /*big_tiff=*/false);
    const std::array<uint8_t, 2> bad_packbits = {0x00, 0xaa};
    const std::array<uint8_t, 4> good_packbits = {0x81, 0xaa, 0x81, 0xaa};
    const uint64_t bad_off = b.AppendBlob(bad_packbits);
    const uint64_t good_off = b.AppendBlob(good_packbits);
    b.AddIfd({
        b.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        b.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        b.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        b.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS),
        b.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        b.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE),
        b.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 2),
        b.MakeLong(TIFFTAG_TILEWIDTH, 16),
        b.MakeLong(TIFFTAG_TILELENGTH, 16),
        b.MakeLongArray(
            TIFFTAG_TILEOFFSETS,
            std::array<uint32_t, 2>{static_cast<uint32_t>(bad_off),
                                    static_cast<uint32_t>(good_off)}),
        b.MakeLongArray(TIFFTAG_TILEBYTECOUNTS, std::array<uint32_t, 2>{2, 4}),
    });
    WriteBytesToFile(err_sep_tile, b.bytes());
  }

  const SubprocessResult sep_tile_stop =
      RunTiffinfo({"-d", err_sep_tile.native()});
  EXPECT_EQ(sep_tile_stop.term_signal, 0);
  EXPECT_EQ(sep_tile_stop.exit_code, 1);
  EXPECT_THAT(sep_tile_stop.stdout_output, Not(HasSubstr("Tile (0,0,1):")));
  EXPECT_THAT(sep_tile_stop.stderr_output, Not(IsEmpty()));

  const SubprocessResult sep_tile_cont =
      RunTiffinfo({"-i", "-d", err_sep_tile.native()});
  EXPECT_EQ(sep_tile_cont.term_signal, 0);
  EXPECT_EQ(sep_tile_cont.exit_code, 1);
  EXPECT_THAT(sep_tile_cont.stdout_output, HasSubstr("Tile (0,0,1):"));
  EXPECT_THAT(sep_tile_cont.stderr_output, Not(IsEmpty()));
}

}  // namespace
}  // namespace libtiff
