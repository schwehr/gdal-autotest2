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

// Unit tests for libtiff directory tag get/set, IFD navigation, custom/EXIF/GPS
// directories, and IFD unlinking/advancing in libtiff/tif_dir.c.
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_dir.c

#include "third_party/tiff/libtiff/tif_dir.h"

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiff.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

int CallVSetField(TIFF* tif, uint32_t tag, ...) {
  va_list ap;
  va_start(ap, tag);
  int status = TIFFVSetField(tif, tag, ap);
  va_end(ap);
  return status;
}

int CallVGetField(TIFF* tif, uint32_t tag, ...) {
  va_list ap;
  va_start(ap, tag);
  int status = TIFFVGetField(tif, tag, ap);
  va_end(ap);
  return status;
}

int CallVGetFieldDefaulted(TIFF* tif, uint32_t tag, ...) {
  va_list ap;
  va_start(ap, tag);
  int status = TIFFVGetFieldDefaulted(tif, tag, ap);
  va_end(ap);
  return status;
}

void WriteMinimalStripImage(TIFF* tif, uint32_t width, uint32_t height,
                            uint8_t fill_byte) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, height), 1);

  std::vector<uint8_t> row(width, fill_byte);
  for (uint32_t r = 0; r < height; ++r) {
    ASSERT_EQ(TIFFWriteScanline(tif, row.data(), r, 0), 1);
  }
}

std::vector<RawIfdEntry> MakeMinimalClassicOrBigIfdEntries(
    const RawTiffBuilder& builder, uint64_t strip_offset, uint32_t width = 1,
    uint32_t height = 1) {
  return {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.big_tiff()
          ? builder.MakeLong8(TIFFTAG_STRIPOFFSETS, strip_offset)
          : builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                             static_cast<uint32_t>(strip_offset)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, height),
      builder.big_tiff()
          ? builder.MakeLong8(TIFFTAG_STRIPBYTECOUNTS, width * height)
          : builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS, width * height),
  };
}

int FailingCodecInit(TIFF* /*tif*/, int /*scheme*/) { return 0; }

constexpr uint32_t kCustomTagSByte = 65100;
constexpr uint32_t kCustomTagSShort = 65101;
constexpr uint32_t kCustomTagSLong = 65102;
constexpr uint32_t kCustomTagIfd = 65103;
constexpr uint32_t kCustomTagSLong8 = 65104;
constexpr uint32_t kCustomTagLong8Array = 65105;
constexpr uint32_t kCustomTagIfd8Array = 65106;
constexpr uint32_t kCustomTagSLong8Array = 65107;
constexpr uint32_t kCustomTagC32Ascii = 65108;
constexpr uint32_t kCustomTagNoType = 65109;
constexpr uint32_t kCustomTagInvalidSetGet = 65110;

constexpr TIFFField kExtendedFields[] = {
    {kCustomTagSByte, 1, 1, TIFF_SBYTE, 0, TIFF_SETGET_SINT8, FIELD_CUSTOM, 1,
     0, "CustomSByte", nullptr},
    {kCustomTagSShort, 1, 1, TIFF_SSHORT, 0, TIFF_SETGET_SINT16, FIELD_CUSTOM,
     1, 0, "CustomSShort", nullptr},
    {kCustomTagSLong, 1, 1, TIFF_SLONG, 0, TIFF_SETGET_SINT32, FIELD_CUSTOM, 1,
     0, "CustomSLong", nullptr},
    {kCustomTagIfd, 1, 1, TIFF_IFD, 0, TIFF_SETGET_UINT32, FIELD_CUSTOM, 1, 0,
     "CustomIfd", nullptr},
    {kCustomTagSLong8, 1, 1, TIFF_SLONG8, 0, TIFF_SETGET_SINT64, FIELD_CUSTOM,
     1, 0, "CustomSLong8", nullptr},
    {kCustomTagLong8Array, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_LONG8, 0,
     TIFF_SETGET_C16_UINT64, FIELD_CUSTOM, 1, 1, "CustomLong8Array", nullptr},
    {kCustomTagIfd8Array, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_IFD8, 0,
     TIFF_SETGET_C16_IFD8, FIELD_CUSTOM, 1, 1, "CustomIfd8Array", nullptr},
    {kCustomTagSLong8Array, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_SLONG8, 0,
     TIFF_SETGET_C16_SINT64, FIELD_CUSTOM, 1, 1, "CustomSLong8Array", nullptr},
    {kCustomTagC32Ascii, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_ASCII, 0,
     TIFF_SETGET_C32_ASCII, FIELD_CUSTOM, 1, 1, "CustomC32Ascii", nullptr},
    {kCustomTagNoType, 1, 1, TIFF_NOTYPE, 0, TIFF_SETGET_UNDEFINED,
     FIELD_CUSTOM, 1, 0, "CustomNoType", nullptr},
    {kCustomTagInvalidSetGet, 1, 1, TIFF_BYTE, 0,
     static_cast<TIFFSetGetFieldType>(60), FIELD_CUSTOM, 1, 0,
     "CustomInvalidSetGet", nullptr},
};

void MergeExtendedFieldsProc(TIFF* tif) {
  _TIFFMergeFields(tif, kExtendedFields, TIFFArrayCount(kExtendedFields));
}

constexpr uint32_t kLegacyCompatTag = 65120;
constexpr TIFFFieldInfo kLegacyFieldInfos[] = {
    {kLegacyCompatTag, 1, 1, TIFF_LONG, FIELD_CUSTOM, 1, 0, "LegacyCompatLong"},
};

void MergeLegacyFieldInfoProc(TIFF* tif) {
  TIFFMergeFieldInfo(tif, kLegacyFieldInfos, TIFFArrayCount(kLegacyFieldInfos));
}

constexpr uint32_t kCustomIfdTagShort = 65200;
constexpr uint32_t kCustomIfdTagAscii = 65201;
constexpr TIFFField kCustomDirFields[] = {
    {kCustomIfdTagShort, 1, 1, TIFF_SHORT, 0, TIFF_SETGET_UINT16, FIELD_CUSTOM,
     1, 0, "CustomDirShort", nullptr},
    {kCustomIfdTagAscii, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_ASCII, 0,
     TIFF_SETGET_ASCII, FIELD_CUSTOM, 1, 0, "CustomDirAscii", nullptr},
};
constexpr TIFFFieldArray kCustomDirFieldArray = {
    tfiatOther, 0, TIFFArrayCount(kCustomDirFields),
    const_cast<TIFFField*>(kCustomDirFields)};

TEST(TifDirTest, InternalArraySettersAndCleanup) {
  void* byte_buf = nullptr;
  auto free_bytes = absl::MakeCleanup([&byte_buf] { _TIFFfree(byte_buf); });
  const std::array<uint8_t, 4> kBytes1 = {1, 2, 3, 4};
  const std::array<uint8_t, 2> kBytes2 = {9, 8};
  _TIFFsetByteArray(&byte_buf, kBytes1.data(), kBytes1.size());
  ASSERT_NE(byte_buf, nullptr);
  EXPECT_EQ(std::memcmp(byte_buf, kBytes1.data(), kBytes1.size()), 0);
  _TIFFsetByteArray(&byte_buf, kBytes2.data(), kBytes2.size());
  ASSERT_NE(byte_buf, nullptr);
  EXPECT_EQ(std::memcmp(byte_buf, kBytes2.data(), kBytes2.size()), 0);
  _TIFFsetByteArray(&byte_buf, kBytes2.data(), 0);
  EXPECT_EQ(byte_buf, nullptr);
  _TIFFsetByteArray(&byte_buf, nullptr, 4);
  EXPECT_EQ(byte_buf, nullptr);

  uint16_t* short_buf = nullptr;
  auto free_shorts = absl::MakeCleanup([&short_buf] { _TIFFfree(short_buf); });
  const std::array<uint16_t, 3> kShorts = {100, 200, 300};
  _TIFFsetShortArray(&short_buf, kShorts.data(), kShorts.size());
  ASSERT_NE(short_buf, nullptr);
  EXPECT_EQ(short_buf[2], 300);
  _TIFFsetShortArray(&short_buf, nullptr, 0);
  EXPECT_EQ(short_buf, nullptr);

  uint32_t* long_buf = nullptr;
  auto free_longs = absl::MakeCleanup([&long_buf] { _TIFFfree(long_buf); });
  const std::array<uint32_t, 2> kLongs = {0x12345678U, 0x9abcdef0U};
  _TIFFsetLongArray(&long_buf, kLongs.data(), kLongs.size());
  ASSERT_NE(long_buf, nullptr);
  EXPECT_EQ(long_buf[1], 0x9abcdef0U);

  float* float_buf = nullptr;
  auto free_floats = absl::MakeCleanup([&float_buf] { _TIFFfree(float_buf); });
  const std::array<float, 2> kFloats = {1.25f, -3.5f};
  _TIFFsetFloatArray(&float_buf, kFloats.data(), kFloats.size());
  ASSERT_NE(float_buf, nullptr);
  EXPECT_FLOAT_EQ(float_buf[1], -3.5f);

  double* double_buf = nullptr;
  auto free_doubles =
      absl::MakeCleanup([&double_buf] { _TIFFfree(double_buf); });
  const std::array<double, 2> kDoubles = {6.25, -12.5};
  _TIFFsetDoubleArray(&double_buf, kDoubles.data(), kDoubles.size());
  ASSERT_NE(double_buf, nullptr);
  EXPECT_DOUBLE_EQ(double_buf[0], 6.25);

  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  void* ext_bytes = nullptr;
  uint32_t* ext_longs = nullptr;
  auto free_ext = absl::MakeCleanup([&] {
    _TIFFfreeExt(tif.get(), ext_bytes);
    _TIFFfreeExt(tif.get(), ext_longs);
  });
  _TIFFsetByteArrayExt(tif.get(), &ext_bytes, kBytes1.data(), kBytes1.size());
  ASSERT_NE(ext_bytes, nullptr);
  _TIFFsetLongArrayExt(tif.get(), &ext_longs, kLongs.data(), kLongs.size());
  ASSERT_NE(ext_longs, nullptr);
  EXPECT_EQ(ext_longs[0], 0x12345678U);
}

TEST(TifDirTest, StandardScalarAndPairFieldsRoundTrip) {
  for (const char* mode : {"wl", "wb", "w8l", "w8b"}) {
    SCOPED_TRACE(mode);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(mode);
      ASSERT_NE(writer, nullptr);

      // Test RowsPerStrip before and after TileDimensions are set.
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{32}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{16}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{8}),
                1);
      EXPECT_EQ(writer->tif_dir.td_tilelength, 8U);
      EXPECT_EQ(writer->tif_dir.td_tilewidth, 32U);

      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SUBFILETYPE,
                             FILETYPE_REDUCEDIMAGE),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_THRESHHOLDING,
                             THRESHHOLD_HALFTONE),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_FILLORDER, FILLORDER_LSB2MSB), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_ORIENTATION, ORIENTATION_BOTLEFT),
          1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_MINSAMPLEVALUE, uint16_t{1}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_MAXSAMPLEVALUE, uint16_t{250}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_XRESOLUTION, 150.5), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YRESOLUTION, 300.25), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             PLANARCONFIG_SEPARATE),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_XPOSITION, 1.5), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YPOSITION, 2.5), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_RESOLUTIONUNIT,
                             RESUNIT_CENTIMETER),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PAGENUMBER, uint16_t{1},
                             uint16_t{4}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_HALFTONEHINTS, uint16_t{10},
                             uint16_t{240}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YCBCRPOSITIONING,
                             YCBCRPOSITION_COSITED),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YCBCRSUBSAMPLING,
                             uint16_t{2}, uint16_t{1}),
                1);
      ASSERT_EQ(CallVSetField(writer.get(), TIFFTAG_IMAGEDEPTH, uint32_t{2}),
                1);

      std::vector<uint8_t> strip(32 * 8, 0x5A);
      for (uint32_t s = 0; s < 6; ++s) {
        ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), s, strip.data(),
                                        static_cast<tmsize_t>(strip.size())),
                  static_cast<tmsize_t>(strip.size()));
      }
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    uint32_t u32 = 0;
    uint16_t u16a = 0;
    uint16_t u16b = 0;
    float fval = 0.0f;
    const uint64_t* striles = nullptr;

    EXPECT_EQ(CallVGetField(reader.get(), TIFFTAG_SUBFILETYPE, &u32), 1);
    EXPECT_EQ(u32, FILETYPE_REDUCEDIMAGE);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGEWIDTH, &u32), 1);
    EXPECT_EQ(u32, 32U);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGELENGTH, &u32), 1);
    EXPECT_EQ(u32, 16U);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_BITSPERSAMPLE, &u16a), 1);
    EXPECT_EQ(u16a, 8);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_COMPRESSION, &u16a), 1);
    EXPECT_EQ(u16a, COMPRESSION_NONE);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_PHOTOMETRIC, &u16a), 1);
    EXPECT_EQ(u16a, PHOTOMETRIC_YCBCR);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_THRESHHOLDING, &u16a), 1);
    EXPECT_EQ(u16a, THRESHHOLD_HALFTONE);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_FILLORDER, &u16a), 1);
    EXPECT_EQ(u16a, FILLORDER_LSB2MSB);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_ORIENTATION, &u16a), 1);
    EXPECT_EQ(u16a, ORIENTATION_BOTLEFT);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_SAMPLESPERPIXEL, &u16a), 1);
    EXPECT_EQ(u16a, 3);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_ROWSPERSTRIP, &u32), 1);
    EXPECT_EQ(u32, 8U);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_MINSAMPLEVALUE, &u16a), 1);
    EXPECT_EQ(u16a, 1);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_MAXSAMPLEVALUE, &u16a), 1);
    EXPECT_EQ(u16a, 250);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_XRESOLUTION, &fval), 1);
    EXPECT_FLOAT_EQ(fval, 150.5f);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_YRESOLUTION, &fval), 1);
    EXPECT_FLOAT_EQ(fval, 300.25f);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_PLANARCONFIG, &u16a), 1);
    EXPECT_EQ(u16a, PLANARCONFIG_SEPARATE);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_XPOSITION, &fval), 1);
    EXPECT_FLOAT_EQ(fval, 1.5f);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_YPOSITION, &fval), 1);
    EXPECT_FLOAT_EQ(fval, 2.5f);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_RESOLUTIONUNIT, &u16a), 1);
    EXPECT_EQ(u16a, RESUNIT_CENTIMETER);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_PAGENUMBER, &u16a, &u16b), 1);
    EXPECT_EQ(u16a, 1);
    EXPECT_EQ(u16b, 4);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_HALFTONEHINTS, &u16a, &u16b),
              1);
    EXPECT_EQ(u16a, 10);
    EXPECT_EQ(u16b, 240);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_YCBCRPOSITIONING, &u16a), 1);
    EXPECT_EQ(u16a, YCBCRPOSITION_COSITED);
    EXPECT_EQ(
        TIFFGetField(reader.get(), TIFFTAG_YCBCRSUBSAMPLING, &u16a, &u16b), 1);
    EXPECT_EQ(u16a, 2);
    EXPECT_EQ(u16b, 1);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGEDEPTH, &u32), 1);
    EXPECT_EQ(u32, 2U);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_STRIPOFFSETS, &striles), 1);
    EXPECT_NE(striles, nullptr);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_STRIPBYTECOUNTS, &striles), 1);
    EXPECT_NE(striles, nullptr);
  }
}

TEST(TifDirTest, TiledFieldsRoundTripAndRowsPerStripInteraction) {
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{32}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{32}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, uint32_t{16}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, uint32_t{16}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEDEPTH, uint32_t{2}), 1);
    // Setting RowsPerStrip after TileDimensions must not clobber
    // TileWidth/Length.
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{4}), 1);
    EXPECT_EQ(writer->tif_dir.td_tilewidth, 16U);
    EXPECT_EQ(writer->tif_dir.td_tilelength, 16U);

    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    const tmsize_t tile_size = TIFFTileSize(writer.get());
    ASSERT_EQ(tile_size, 16 * 16);
    std::vector<uint8_t> tile(static_cast<size_t>(tile_size), 0x11);
    for (uint32_t t = 0; t < TIFFNumberOfTiles(writer.get()); ++t) {
      ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), t, tile.data(), tile_size),
                tile_size);
    }
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  uint32_t tw = 0;
  uint32_t tl = 0;
  uint32_t td = 0;
  const uint64_t* offsets = nullptr;
  const uint64_t* counts = nullptr;
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_TILEWIDTH, &tw), 1);
  EXPECT_EQ(tw, 16U);
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_TILELENGTH, &tl), 1);
  EXPECT_EQ(tl, 16U);
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_TILEDEPTH, &td), 1);
  EXPECT_EQ(td, 2U);
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_TILEOFFSETS, &offsets), 1);
  EXPECT_NE(offsets, nullptr);
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_TILEBYTECOUNTS, &counts), 1);
  EXPECT_NE(counts, nullptr);
}

TEST(TifDirTest, BigEndianBitsPerSampleAndComplexPostDecode) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("wb");
  ASSERT_NE(tif, nullptr);
  ASSERT_NE(TIFFIsByteSwapped(tif.get()), 0);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
  EXPECT_EQ(tif->tif_postdecode, _TIFFNoPostDecode);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{16}), 1);
  EXPECT_EQ(tif->tif_postdecode, _TIFFSwab16BitData);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{24}), 1);
  EXPECT_EQ(tif->tif_postdecode, _TIFFSwab24BitData);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{32}), 1);
  EXPECT_EQ(tif->tif_postdecode, _TIFFSwab32BitData);
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_COMPLEXINT),
      1);
  EXPECT_EQ(tif->tif_postdecode, _TIFFSwab16BitData);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{64}), 1);
  EXPECT_EQ(tif->tif_postdecode, _TIFFSwab64BitData);
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_COMPLEXIEEEFP),
      1);
  EXPECT_EQ(tif->tif_postdecode, _TIFFSwab32BitData);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{128}), 1);
  EXPECT_EQ(tif->tif_postdecode, _TIFFSwab64BitData);
}

TEST(TifDirTest, SampleFormatAndDataTypeMappings) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  // Backward-compatibility DATATYPE_* constants defined internally in
  // tif_dir.c.
  constexpr uint16_t kDataTypeVoid = 0;
  constexpr uint16_t kDataTypeInt = 1;
  constexpr uint16_t kDataTypeUint = 2;
  constexpr uint16_t kDataTypeIeeeFp = 3;

  struct DataTypeMapping {
    uint16_t datatype;
    uint16_t sampleformat;
  };
  constexpr std::array<DataTypeMapping, 4> kMappings = {{
      {.datatype = kDataTypeVoid, .sampleformat = SAMPLEFORMAT_VOID},
      {.datatype = kDataTypeInt, .sampleformat = SAMPLEFORMAT_INT},
      {.datatype = kDataTypeUint, .sampleformat = SAMPLEFORMAT_UINT},
      {.datatype = kDataTypeIeeeFp, .sampleformat = SAMPLEFORMAT_IEEEFP},
  }};
  for (const auto& m : kMappings) {
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_DATATYPE, m.datatype), 1);
    uint16_t sf = 0;
    uint16_t dt = 99;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SAMPLEFORMAT, &sf), 1);
    EXPECT_EQ(sf, m.sampleformat);
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_DATATYPE, &dt), 1);
    EXPECT_EQ(dt, m.datatype);
  }
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_DATATYPE, uint16_t{99}), 0);

  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_COMPLEXINT),
      1);
  uint16_t sf = 0;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SAMPLEFORMAT, &sf), 1);
  EXPECT_EQ(sf, SAMPLEFORMAT_COMPLEXINT);
  // Pre-init dt so hitting default branch in _TIFFVGetField leaves no uninit.
  uint16_t dt = 42;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_DATATYPE, &dt), 1);
  EXPECT_EQ(dt, 42);

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLEFORMAT, uint16_t{0}), 0);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLEFORMAT, uint16_t{7}), 0);
}

TEST(TifDirTest, SMinAndSMaxSampleValueMergedAndPerSample) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);

  // Merged mode sets all samples to the same scalar value.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SMINSAMPLEVALUE, -5.5), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SMAXSAMPLEVALUE, 255.5), 1);
  double min_scalar = 0.0;
  double max_scalar = 0.0;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SMINSAMPLEVALUE, &min_scalar), 1);
  EXPECT_DOUBLE_EQ(min_scalar, -5.5);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SMAXSAMPLEVALUE, &max_scalar), 1);
  EXPECT_DOUBLE_EQ(max_scalar, 255.5);

  // Multi-sample mode sets/gets double arrays.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PERSAMPLE, PERSAMPLE_MULTI), 1);
  const std::array<double, 3> kMins = {5.0, 1.25, 3.0};
  const std::array<double, 3> kMaxs = {10.0, 42.5, 20.0};
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SMINSAMPLEVALUE, kMins.data()), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SMAXSAMPLEVALUE, kMaxs.data()), 1);
  double* got_mins = nullptr;
  double* got_maxs = nullptr;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SMINSAMPLEVALUE, &got_mins), 1);
  ASSERT_NE(got_mins, nullptr);
  EXPECT_DOUBLE_EQ(got_mins[1], 1.25);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SMAXSAMPLEVALUE, &got_maxs), 1);
  ASSERT_NE(got_maxs, nullptr);
  EXPECT_DOUBLE_EQ(got_maxs[1], 42.5);

  // Switching back to merged mode computes aggregate min/max across samples.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PERSAMPLE, PERSAMPLE_MERGED), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SMINSAMPLEVALUE, &min_scalar), 1);
  EXPECT_DOUBLE_EQ(min_scalar, 1.25);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SMAXSAMPLEVALUE, &max_scalar), 1);
  EXPECT_DOUBLE_EQ(max_scalar, 42.5);

  // Changing SamplesPerPixel cancels SMin/SMaxSampleValue with warnings.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{4}), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SMINSAMPLEVALUE, &min_scalar), 0);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SMAXSAMPLEVALUE, &max_scalar), 0);
  EXPECT_GE(stream.diagnostics().warning_count(), 2U);
}

TEST(TifDirTest, ColormapTransferFunctionAndReferenceBlackWhite) {
  MemoryTiffStream stream;
  {
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{32}), 1);
    std::array<uint16_t, 16> dummy{};
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COLORMAP, dummy.data(),
                           dummy.data(), dummy.data()),
              0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_TRANSFERFUNCTION, dummy.data()),
              0);

    // 4-bit grayscale with 1-table TransferFunction.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{4}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
    std::array<uint16_t, 16> tf0{};
    for (uint16_t i = 0; i < 16; ++i) {
      tf0[i] = static_cast<uint16_t>(i * 4096U);
    }
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TRANSFERFUNCTION, tf0.data()), 1);
    const uint16_t* got_tf[3] = {nullptr, nullptr, nullptr};
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_TRANSFERFUNCTION, &got_tf[0],
                           &got_tf[1], &got_tf[2]),
              1);
    EXPECT_NE(got_tf[0], nullptr);
    EXPECT_EQ(got_tf[1], nullptr);
    EXPECT_EQ(got_tf[2], nullptr);

    // Increasing SamplesPerPixel from 1 to 3 cancels 1-table TransferFunction.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_TRANSFERFUNCTION, &got_tf[0],
                           &got_tf[1], &got_tf[2]),
              0);

    // Set 3-table TransferFunction and verify it cancels when ExtraSamples
    // drops from 1 to 0 on a 2-sample image.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{2}), 1);
    std::array<uint16_t, 1> extra_alpha = {EXTRASAMPLE_UNASSALPHA};
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_EXTRASAMPLES, uint16_t{1},
                           extra_alpha.data()),
              1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TRANSFERFUNCTION, tf0.data()), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_EXTRASAMPLES, uint16_t{0},
                           static_cast<uint16_t*>(nullptr)),
              1);
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_TRANSFERFUNCTION, &got_tf[0],
                           &got_tf[1], &got_tf[2]),
              0);

    // Full 3-channel TransferFunction + ColorMap + ReferenceBlackWhite.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
    std::array<uint16_t, 16> tf1{};
    std::array<uint16_t, 16> tf2{};
    tf1.fill(123);
    tf2.fill(456);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TRANSFERFUNCTION, tf0.data(),
                           tf1.data(), tf2.data()),
              1);
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_TRANSFERFUNCTION, &got_tf[0],
                           &got_tf[1], &got_tf[2]),
              1);
    EXPECT_EQ(got_tf[1][0], 123);
    EXPECT_EQ(got_tf[2][0], 456);

    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_PALETTE),
              1);
    std::array<uint16_t, 16> cmap_r{};
    std::array<uint16_t, 16> cmap_g{};
    std::array<uint16_t, 16> cmap_b{};
    for (uint16_t i = 0; i < 16; ++i) {
      cmap_r[i] = static_cast<uint16_t>(i * 100);
      cmap_g[i] = static_cast<uint16_t>(i * 200);
      cmap_b[i] = static_cast<uint16_t>(i * 300);
    }
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COLORMAP, cmap_r.data(),
                           cmap_g.data(), cmap_b.data()),
              1);
    const uint16_t* got_cmap[3] = {nullptr, nullptr, nullptr};
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_COLORMAP, &got_cmap[0],
                           &got_cmap[1], &got_cmap[2]),
              1);
    EXPECT_EQ(got_cmap[0][5], 500);
    EXPECT_EQ(got_cmap[1][5], 1000);
    EXPECT_EQ(got_cmap[2][5], 1500);

    const std::array<float, 6> kRbw = {0.0f,   255.0f, 16.0f,
                                       235.0f, 16.0f,  240.0f};
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_REFERENCEBLACKWHITE, kRbw.data()),
              1);
    const float* got_rbw = nullptr;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_REFERENCEBLACKWHITE, &got_rbw),
              1);
    ASSERT_NE(got_rbw, nullptr);
    EXPECT_FLOAT_EQ(got_rbw[3], 235.0f);

    // Exercise TIFFFreeDirectory cleanup of all allocated tag arrays.
    EXPECT_EQ(TIFFCreateDirectory(tif.get()), 0);
  }
}

TEST(TifDirTest, ExtraSamplesAndMatteing) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);

  std::array<uint16_t, 2> extras = {EXTRASAMPLE_ASSOCALPHA, 999};
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_EXTRASAMPLES, uint16_t{2}, extras.data()),
      1);
  // Corel Draw 999 value is normalized to EXTRASAMPLE_UNASSALPHA.
  EXPECT_EQ(extras[1], EXTRASAMPLE_UNASSALPHA);
  uint16_t count = 0;
  const uint16_t* sampleinfo = nullptr;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_EXTRASAMPLES, &count, &sampleinfo),
            1);
  EXPECT_EQ(count, 2);
  EXPECT_EQ(sampleinfo[0], EXTRASAMPLE_ASSOCALPHA);
  EXPECT_EQ(sampleinfo[1], EXTRASAMPLE_UNASSALPHA);

  uint16_t matteing = 1;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_MATTEING, &matteing), 1);
  EXPECT_EQ(matteing, 0);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_MATTEING, uint16_t{1}), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_MATTEING, &matteing), 1);
  EXPECT_EQ(matteing, 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_MATTEING, uint16_t{0}), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_MATTEING, &matteing), 1);
  EXPECT_EQ(matteing, 0);

  // Rejections in setExtraSamples.
  std::array<uint16_t, 4> too_many = {0, 1, 2, 0};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_EXTRASAMPLES, uint16_t{4},
                         too_many.data()),
            0);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_EXTRASAMPLES, uint16_t{1},
                         static_cast<uint16_t*>(nullptr)),
            0);
  std::array<uint16_t, 1> bad_extra = {3};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_EXTRASAMPLES, uint16_t{1},
                         bad_extra.data()),
            0);
}

TEST(TifDirTest, CmykInkNamesAndNumberOfInks) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{4}), 1);

  // Invalid InkNames: zero length and missing trailing NUL.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_INKNAMES, uint16_t{0}, ""), 0);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_INKNAMES, uint16_t{4}, "Cyan"), 0);

  // Set NumberOfInks first with mismatching count against SamplesPerPixel (4).
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_NUMBEROFINKS, uint16_t{3}), 1);
  uint16_t ninks = 0;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_NUMBEROFINKS, &ninks), 1);
  EXPECT_EQ(ninks, 3);

  // Setting 4-ink InkNames overrides NumberOfInks from 3 -> 4.
  constexpr char kInks4[] = "Cyan\0Magenta\0Yellow\0Black";
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_INKNAMES,
                         static_cast<uint16_t>(sizeof(kInks4)), kInks4),
            1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_NUMBEROFINKS, &ninks), 1);
  EXPECT_EQ(ninks, 4);
  const char* got_inks = nullptr;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_INKNAMES, &got_inks), 1);
  ASSERT_NE(got_inks, nullptr);
  EXPECT_EQ(std::memcmp(got_inks, kInks4, sizeof(kInks4)), 0);

  // Changing NumberOfInks after InkNames to a mismatching value must fail.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_NUMBEROFINKS, uint16_t{2}), 0);

  // Fresh directory: InkNames sets NumberOfInks automatically and warns if
  // SamplesPerPixel differs.
  ASSERT_EQ(TIFFCreateDirectory(tif.get()), 0);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_INKNAMES,
                         static_cast<uint16_t>(sizeof(kInks4)), kInks4),
            1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_NUMBEROFINKS, &ninks), 1);
  EXPECT_EQ(ninks, 4);
}

TEST(TifDirTest, InvalidValuesOkToChangeAndCodecIgnoredTags) {
  constexpr uint16_t kFailingScheme = 55555;
  ScopedRegisteredCodec failing_codec(kFailingScheme, "TestFailCodec",
                                      &FailingCodecInit);
  MemoryTiffStream stream;
  {
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);

    // Unknown standard tag and unknown pseudo-tag.
    EXPECT_EQ(TIFFSetField(tif.get(), 64999U, uint32_t{1}), 0);
    EXPECT_EQ(TIFFSetField(tif.get(), 99999U, uint32_t{1}), 0);
    uint32_t dummy32 = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), 64999U, &dummy32), 0);
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PERSAMPLE, &dummy32), 0);

    // Invalid scalar field values.
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_FILLORDER, uint16_t{3}), 0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ORIENTATION, uint16_t{0}), 0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ORIENTATION, uint16_t{9}), 0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{0}), 0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{0}), 0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_XRESOLUTION, -1.0), 0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_XRESOLUTION,
                           std::numeric_limits<double>::quiet_NaN()),
              0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_YRESOLUTION, -2.0), 0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, uint16_t{3}), 0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_RESOLUTIONUNIT, uint16_t{0}), 0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_RESOLUTIONUNIT, uint16_t{4}), 0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, uint32_t{10}), 0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, uint32_t{10}), 0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEDEPTH, uint32_t{0}), 0);

    // Compression switching: same-scheme no-op, cleanup on change, failing
    // init.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_LZW), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_LZW), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL),
              1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_DEFLATE),
              1);
    // Test registered codec whose init fails while td_compression != NONE so
    // switching to COMPRESSION_NONE next re-initializes DumpMode.
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, kFailingScheme), 0);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);

    // Predictor remains in tif_fields with FIELD_PREDICTOR set, so
    // _TIFFVSetField and _TIFFVGetField reject it as unsupported by current
    // codec.
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PREDICTOR, PREDICTOR_NONE), 0);
    uint16_t pred = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PREDICTOR, &pred), 0);

    // FIELD_IGNORE tag and pseudo-tag left over after JPEG cleanup.
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_OSUBFILETYPE, uint16_t{1}), 0);
    if (TIFFIsCODECConfigured(COMPRESSION_JPEG)) {
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
                1);
      EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGQUALITY, 80), 0);
      int q = 0;
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_JPEGQUALITY, &q), 0);
    }

    // OkToChangeTag after writing starts: reset stale codec field bits first,
    // then ImageWidth is blocked while ImageLength and Artist are allowed.
    ASSERT_EQ(TIFFCreateDirectory(tif.get()), 0);
    WriteMinimalStripImage(tif.get(), 4, 2, 0x22);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{8}), 0);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{2}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ARTIST, "Editable"), 1);
    ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
  }

  // Non-multiple-of-16 TileWidth/TileLength in O_RDONLY warns and succeeds.
  UniqueTiff rd = stream.Open("r");
  ASSERT_NE(rd, nullptr);
  EXPECT_EQ(TIFFSetField(rd.get(), TIFFTAG_TILEWIDTH, uint32_t{10}), 1);
  EXPECT_EQ(TIFFSetField(rd.get(), TIFFTAG_TILELENGTH, uint32_t{12}), 1);
}

TEST(TifDirTest, CustomTagsDotRangeUnsetAndClassicVsBigTiff) {
  // ClassicTIFF tests.
  {
    MemoryTiffStream stream;
    {
      UniqueTiff tif = stream.Open("w");
      ASSERT_NE(tif, nullptr);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{2}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{2}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
                1);

      // Scalar custom types.
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ARTIST, "InitialArtist"), 1);
      // Overwrite existing custom tag to cover value replacement path.
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ARTIST, "UpdatedArtist"), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_MAKE, "TestMake"), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_MODEL, "TestModel"), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_FAXPROFILE, FAXPROFILE_S), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_CELLWIDTH, uint16_t{7}), 1);
      ASSERT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_PROFILETYPE, PROFILETYPE_G3_FAX), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PIXAR_FOVCOT, 1.75), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_STONITS, 12.345), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BASELINEEXPOSURE, -1.25), 1);
      ASSERT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_INTEROPERABILITYIFD, uint64_t{1024}),
          1);

      // DotRange special pair.
      ASSERT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_DOTRANGE, uint16_t{5}, uint16_t{250}),
          1);

      // C0 fixed arrays.
      const std::array<uint8_t, 4> kDngVer = {1, 4, 0, 0};
      const std::array<float, 2> kWhitePt = {0.3127f, 0.3290f};
      const std::array<uint16_t, 2> kBlackDim = {2, 2};
      const std::array<uint32_t, 4> kActiveArea = {0, 0, 2, 2};
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_DNGVERSION, kDngVer.data()), 1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_WHITEPOINT, kWhitePt.data()),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BLACKLEVELREPEATDIM,
                             kBlackDim.data()),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ACTIVEAREA, kActiveArea.data()),
                1);

      // C16 and C32 variable arrays.
      const std::array<uint8_t, 3> kPriv = {0xAA, 0xBB, 0xCC};
      const std::array<uint16_t, 2> kLinTbl = {10, 200};
      const std::array<uint32_t, 1> kWhiteLvl = {255};
      const std::array<float, 4> kBlackLvl = {1.0f, 2.0f, 3.0f, 4.0f};
      const std::array<uint8_t, 5> kXml = {'<', 'x', '/', '>', '\0'};
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_DNGPRIVATEDATA, uint16_t{3},
                             kPriv.data()),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_LINEARIZATIONTABLE, uint16_t{2},
                             kLinTbl.data()),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_WHITELEVEL, uint16_t{1},
                             kWhiteLvl.data()),
                1);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BLACKLEVEL, uint16_t{4},
                             kBlackLvl.data()),
                1);
      ASSERT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_XMLPACKET, uint32_t{5}, kXml.data()),
          1);

      // Null count on passcount custom tag warns and succeeds cleanly.
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ICCPROFILE, uint32_t{0},
                             static_cast<uint8_t*>(nullptr)),
                1);

      // ClassicTIFF rejects > 32-bit values for LONG8 / IFD8 custom tags.
      // Note: badvalueifd8long8 deletes an existing entry if overwrite fails,
      // so test EXIFIFD / GPSIFD while leaving INTEROPERABILITYIFD intact.
      EXPECT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_EXIFIFD, uint64_t{0x100000001ULL}),
          0);
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_GPSIFD, uint64_t{2048}), 1);
      EXPECT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_GPSIFD, uint64_t{0x100000001ULL}), 0);
      uint64_t gps_ifd = 0;
      EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_GPSIFD, &gps_ifd), 0);

      // Tag list inspection and TIFFUnsetField.
      const int initial_count = TIFFGetTagListCount(tif.get());
      EXPECT_GT(initial_count, 5);
      EXPECT_EQ(TIFFGetTagListEntry(tif.get(), -1), static_cast<uint32_t>(-1));
      EXPECT_EQ(TIFFGetTagListEntry(tif.get(), initial_count),
                static_cast<uint32_t>(-1));
      EXPECT_EQ(TIFFGetTagListEntry(tif.get(), 0), TIFFTAG_ARTIST);

      EXPECT_EQ(TIFFUnsetField(tif.get(), 64999U), 0);
      EXPECT_EQ(TIFFUnsetField(tif.get(), TIFFTAG_MAKE), 1);
      EXPECT_EQ(TIFFUnsetField(tif.get(), TIFFTAG_COPYRIGHT), 1);
      EXPECT_EQ(TIFFUnsetField(tif.get(), TIFFTAG_SUBFILETYPE), 1);
      EXPECT_EQ(TIFFGetTagListCount(tif.get()), initial_count - 1);

      std::array<uint8_t, 2> row = {1, 2};
      ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), 1);
      ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 1, 0), 1);
      ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
    }

    UniqueTiff rd = stream.Open("r");
    ASSERT_NE(rd, nullptr);
    const char* str_val = nullptr;
    uint8_t u8 = 0;
    uint16_t u16 = 0;
    uint16_t u16b = 0;
    uint32_t u32 = 0;
    float fval = 0.0f;
    double dval = 0.0;
    uint64_t u64 = 0;
    const uint8_t* p_u8 = nullptr;
    const uint16_t* p_u16 = nullptr;
    const uint32_t* p_u32 = nullptr;
    const float* p_f = nullptr;

    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_ARTIST, &str_val), 1);
    EXPECT_STREQ(str_val, "UpdatedArtist");
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_MAKE, &str_val), 0);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_FAXPROFILE, &u8), 1);
    EXPECT_EQ(u8, FAXPROFILE_S);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_CELLWIDTH, &u16), 1);
    EXPECT_EQ(u16, 7);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_PROFILETYPE, &u32), 1);
    EXPECT_EQ(u32, PROFILETYPE_G3_FAX);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_PIXAR_FOVCOT, &fval), 1);
    EXPECT_FLOAT_EQ(fval, 1.75f);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_STONITS, &dval), 1);
    EXPECT_DOUBLE_EQ(dval, 12.345);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_BASELINEEXPOSURE, &fval), 1);
    EXPECT_FLOAT_EQ(fval, -1.25f);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_INTEROPERABILITYIFD, &u64), 1);
    EXPECT_EQ(u64, 1024ULL);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_DOTRANGE, &u16, &u16b), 1);
    EXPECT_EQ(u16, 5);
    EXPECT_EQ(u16b, 250);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_DNGVERSION, &p_u8), 1);
    EXPECT_EQ(p_u8[1], 4);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_WHITEPOINT, &p_f), 1);
    EXPECT_FLOAT_EQ(p_f[0], 0.3127f);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_BLACKLEVELREPEATDIM, &p_u16), 1);
    EXPECT_EQ(p_u16[0], 2);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_ACTIVEAREA, &p_u32), 1);
    EXPECT_EQ(p_u32[2], 2U);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_DNGPRIVATEDATA, &u16, &p_u8), 1);
    EXPECT_EQ(u16, 3);
    EXPECT_EQ(p_u8[2], 0xCC);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_LINEARIZATIONTABLE, &u16, &p_u16),
              1);
    EXPECT_EQ(u16, 2);
    EXPECT_EQ(p_u16[1], 200);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_WHITELEVEL, &u16, &p_u32), 1);
    EXPECT_EQ(u16, 1);
    EXPECT_EQ(p_u32[0], 255U);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_BLACKLEVEL, &u16, &p_f), 1);
    EXPECT_EQ(u16, 4);
    EXPECT_FLOAT_EQ(p_f[3], 4.0f);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_XMLPACKET, &u32, &p_u8), 1);
    EXPECT_EQ(u32, 5U);
    EXPECT_EQ(p_u8[0], '<');
  }

  // BigTIFF accepts > 32-bit LONG8 / IFD8 custom values.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w8");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_EXIFIFD, uint64_t{0x123456789ULL}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_INTEROPERABILITYIFD,
                           uint64_t{0x987654321ULL}),
              1);
    uint64_t v1 = 0;
    uint64_t v2 = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_EXIFIFD, &v1), 1);
    EXPECT_EQ(v1, 0x123456789ULL);
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_INTEROPERABILITYIFD, &v2), 1);
    EXPECT_EQ(v2, 0x987654321ULL);
  }
}

TEST(TifDirTest, TagExtenderCustomScalarArrayTypesAndOom) {
  {
    ScopedTagExtender extender(&MergeExtendedFieldsProc);
    MemoryTiffStream stream;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    // ClassicTIFF handle for extended custom tags & Classic overflow rejection.
    UniqueTiff tif = stream.Open("w", opts.get());
    ASSERT_NE(tif, nullptr);

    ASSERT_EQ(TIFFSetField(tif.get(), kCustomTagSByte, int8_t{-42}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), kCustomTagSShort, int16_t{-1234}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), kCustomTagSLong, int32_t{-123456}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), kCustomTagIfd, uint32_t{4096}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), kCustomTagSLong8, int64_t{-9999}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), kCustomTagC32Ascii, uint32_t{6}, "hello"),
              1);

    int8_t sb = 0;
    int16_t ss = 0;
    int32_t sl = 0;
    uint32_t ifd32 = 0;
    int64_t sl8 = 0;
    uint32_t ascii_len = 0;
    const char* ascii_ptr = nullptr;
    EXPECT_EQ(TIFFGetField(tif.get(), kCustomTagSByte, &sb), 1);
    EXPECT_EQ(sb, -42);
    EXPECT_EQ(TIFFGetField(tif.get(), kCustomTagSShort, &ss), 1);
    EXPECT_EQ(ss, -1234);
    EXPECT_EQ(TIFFGetField(tif.get(), kCustomTagSLong, &sl), 1);
    EXPECT_EQ(sl, -123456);
    EXPECT_EQ(TIFFGetField(tif.get(), kCustomTagIfd, &ifd32), 1);
    EXPECT_EQ(ifd32, 4096U);
    EXPECT_EQ(TIFFGetField(tif.get(), kCustomTagSLong8, &sl8), 1);
    EXPECT_EQ(sl8, -9999);
    EXPECT_EQ(
        TIFFGetField(tif.get(), kCustomTagC32Ascii, &ascii_len, &ascii_ptr), 1);
    EXPECT_EQ(ascii_len, 6U);
    EXPECT_STREQ(ascii_ptr, "hello");

    // Put Artist after SLong8/arrays to test badvalueifd8long8 gap-closing.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ARTIST, "Tail"), 1);

    // ClassicTIFF out-of-range scalar SLONG8 and array LONG8 / IFD8 / SLONG8.
    EXPECT_EQ(TIFFSetField(tif.get(), kCustomTagSLong8, int64_t{3000000000LL}),
              0);
    EXPECT_EQ(TIFFSetField(tif.get(), kCustomTagSLong8, int64_t{-3000000000LL}),
              0);
    const std::array<uint64_t, 2> kTooBigU64 = {1ULL, 0x100000000ULL};
    const std::array<int64_t, 2> kTooBigS64 = {1LL, 3000000000LL};
    const std::array<int64_t, 2> kTooSmallS64 = {1LL, -3000000000LL};
    EXPECT_EQ(TIFFSetField(tif.get(), kCustomTagLong8Array, uint16_t{2},
                           kTooBigU64.data()),
              0);
    EXPECT_EQ(TIFFSetField(tif.get(), kCustomTagIfd8Array, uint16_t{2},
                           kTooBigU64.data()),
              0);
    EXPECT_EQ(TIFFSetField(tif.get(), kCustomTagSLong8Array, uint16_t{2},
                           kTooBigS64.data()),
              0);
    EXPECT_EQ(TIFFSetField(tif.get(), kCustomTagSLong8Array, uint16_t{2},
                           kTooSmallS64.data()),
              0);

    // Invalid field_type / set_get_field_type and custom-tag OOM paths leave
    // partial entries in td_customValues; exercise them on an O_RDONLY handle
    // so TIFFClose frees cleanly without flushing incomplete custom tags.
    {
      MemoryTiffStream ro_stream;
      {
        UniqueTiff init_tif = ro_stream.Open("w");
        ASSERT_NE(init_tif, nullptr);
        WriteMinimalStripImage(init_tif.get(), 2, 2, 0x01);
        ASSERT_EQ(TIFFWriteDirectory(init_tif.get()), 1);
      }
      UniqueTiff ro_tif = ro_stream.Open("r", opts.get());
      ASSERT_NE(ro_tif, nullptr);
      EXPECT_EQ(TIFFSetField(ro_tif.get(), kCustomTagNoType, uint8_t{1}), 0);
      EXPECT_EQ(TIFFSetField(ro_tif.get(), kCustomTagInvalidSetGet, uint8_t{1}),
                0);
      {
        ScopedSingleMemAllocLimit limit(ro_tif.get(), 1);
        EXPECT_EQ(TIFFSetField(ro_tif.get(), TIFFTAG_MAKE, "OomList"), 0);
      }
      ASSERT_EQ(
          TIFFSetField(ro_tif.get(), TIFFTAG_XMLPACKET, uint32_t{4}, "abcd"),
          1);
      {
        ScopedSingleMemAllocLimit limit(ro_tif.get(), 8);
        std::array<uint8_t, 32> big_xml{};
        EXPECT_EQ(TIFFSetField(ro_tif.get(), TIFFTAG_XMLPACKET, uint32_t{32},
                               big_xml.data()),
                  0);
      }
    }

    // Verify Artist survived gap-closing failures.
    const char* tail = nullptr;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_ARTIST, &tail), 1);
    EXPECT_STREQ(tail, "Tail");

    // BigTIFF accepts 64-bit SLONG8 and 64-bit arrays.
    UniqueTiff big_tif = stream.Open("w8");
    ASSERT_NE(big_tif, nullptr);
    ASSERT_EQ(
        TIFFSetField(big_tif.get(), kCustomTagSLong8, int64_t{-5000000000LL}),
        1);
    ASSERT_EQ(TIFFSetField(big_tif.get(), kCustomTagLong8Array, uint16_t{2},
                           kTooBigU64.data()),
              1);
    ASSERT_EQ(TIFFSetField(big_tif.get(), kCustomTagIfd8Array, uint16_t{2},
                           kTooBigU64.data()),
              1);
    ASSERT_EQ(TIFFSetField(big_tif.get(), kCustomTagSLong8Array, uint16_t{2},
                           kTooBigS64.data()),
              1);
    int64_t got_s64 = 0;
    uint16_t cnt = 0;
    const uint64_t* p_u64 = nullptr;
    const int64_t* p_s64 = nullptr;
    EXPECT_EQ(TIFFGetField(big_tif.get(), kCustomTagSLong8, &got_s64), 1);
    EXPECT_EQ(got_s64, -5000000000LL);
    EXPECT_EQ(TIFFGetField(big_tif.get(), kCustomTagLong8Array, &cnt, &p_u64),
              1);
    EXPECT_EQ(cnt, 2);
    EXPECT_EQ(p_u64[1], 0x100000000ULL);
    EXPECT_EQ(TIFFGetField(big_tif.get(), kCustomTagIfd8Array, &cnt, &p_u64),
              1);
    EXPECT_EQ(p_u64[1], 0x100000000ULL);
    EXPECT_EQ(TIFFGetField(big_tif.get(), kCustomTagSLong8Array, &cnt, &p_s64),
              1);
    EXPECT_EQ(p_s64[1], 3000000000LL);
  }

  // Legacy TIFFMergeFieldInfo extender + second TIFFDefaultDirectory cleanup.
  {
    ScopedTagExtender extender(&MergeLegacyFieldInfoProc);
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), kLegacyCompatTag, uint32_t{77}), 1);
    uint32_t got = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), kLegacyCompatTag, &got), 1);
    EXPECT_EQ(got, 77U);
    // TIFFCreateDirectory invokes TIFFDefaultDirectory, freeing
    // tif_fieldscompat.
    ASSERT_EQ(TIFFCreateDirectory(tif.get()), 0);
    ASSERT_EQ(TIFFSetField(tif.get(), kLegacyCompatTag, uint32_t{88}), 1);
  }
}

TEST(TifDirTest, DirectoryLifecycleCheckpointRewriteSubIfdExifGps) {
  MemoryTiffStream stream;
  uint64_t exif_offset = 0;
  uint64_t gps_offset = 0;
  uint64_t custom_offset = 0;
  uint64_t subifd_offset = 0;
  uint64_t main1_offset = 0;

  {
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);

    // Build EXIF IFD first and write it out.
    ASSERT_EQ(TIFFCreateEXIFDirectory(tif.get()), 0);
    ASSERT_EQ(TIFFSetField(tif.get(), EXIFTAG_EXPOSURETIME, 0.01), 1);
    const std::array<uint16_t, 1> kIso = {400};
    ASSERT_EQ(TIFFSetField(tif.get(), EXIFTAG_ISOSPEEDRATINGS, uint16_t{1},
                           kIso.data()),
              1);
    ASSERT_EQ(TIFFSetField(tif.get(), EXIFTAG_FILESOURCE, uint8_t{3}), 1);
    ASSERT_EQ(TIFFWriteCustomDirectory(tif.get(), &exif_offset), 1);
    EXPECT_GT(exif_offset, 0ULL);

    // Build GPS IFD (exercises Rational2Double 8-byte C0_DOUBLE & DOUBLE!).
    ASSERT_EQ(TIFFCreateGPSDirectory(tif.get()), 0);
    const std::array<uint8_t, 4> kGpsVer = {2, 3, 0, 0};
    const std::array<double, 3> kLat = {37.0, 25.0, 19.25};
    ASSERT_EQ(TIFFSetField(tif.get(), GPSTAG_VERSIONID, kGpsVer.data()), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), GPSTAG_LATITUDEREF, "N"), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), GPSTAG_LATITUDE, kLat.data()), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), GPSTAG_ALTITUDEREF, uint8_t{0}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), GPSTAG_ALTITUDE, 123.5), 1);
    ASSERT_EQ(TIFFWriteCustomDirectory(tif.get(), &gps_offset), 1);
    EXPECT_GT(gps_offset, 0ULL);

    // Build arbitrary custom IFD.
    ASSERT_EQ(TIFFCreateCustomDirectory(tif.get(), &kCustomDirFieldArray), 0);
    ASSERT_EQ(TIFFSetField(tif.get(), kCustomIfdTagShort, uint16_t{42}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), kCustomIfdTagAscii, "CustomVal"), 1);
    ASSERT_EQ(TIFFWriteCustomDirectory(tif.get(), &custom_offset), 1);
    EXPECT_GT(custom_offset, 0ULL);

    // Create Main IFD 0 referencing EXIF, GPS, and 1 SubIFD.
    ASSERT_EQ(TIFFCreateDirectory(tif.get()), 0);
    WriteMinimalStripImage(tif.get(), 4, 4, 0x10);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_EXIFIFD, exif_offset), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_GPSIFD, gps_offset), 1);
    ASSERT_EQ(TIFFCheckpointDirectory(tif.get()), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ARTIST, "Rewritten"), 1);
    std::array<uint64_t, 1> sub_offs = {0};
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SUBIFD, uint16_t{1}, sub_offs.data()),
        1);
    ASSERT_EQ(TIFFRewriteDirectory(tif.get()), 1);

    // Now we are in SubIFD mode (TIFF_INSUBIFD is set).
    ASSERT_NE(tif->tif_flags & TIFF_INSUBIFD, 0U);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SUBIFD, uint16_t{1}, sub_offs.data()),
        0);
    WriteMinimalStripImage(tif.get(), 2, 2, 0x20);
    ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);

    // Main IFD 1.
    WriteMinimalStripImage(tif.get(), 3, 3, 0x30);
    ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
  }

  UniqueTiff rd = stream.Open("r");
  ASSERT_NE(rd, nullptr);
  EXPECT_EQ(TIFFNumberOfDirectories(rd.get()), 2);
  ASSERT_EQ(TIFFSetDirectory(rd.get(), 0), 1);
  EXPECT_EQ(TIFFLastDirectory(rd.get()), 0);

  uint16_t nsub = 0;
  const uint64_t* got_subs = nullptr;
  ASSERT_EQ(TIFFGetField(rd.get(), TIFFTAG_SUBIFD, &nsub, &got_subs), 1);
  ASSERT_EQ(nsub, 1);
  subifd_offset = got_subs[0];
  EXPECT_GT(subifd_offset, 0ULL);

  ASSERT_EQ(TIFFGetField(rd.get(), TIFFTAG_EXIFIFD, &exif_offset), 1);
  ASSERT_EQ(TIFFGetField(rd.get(), TIFFTAG_GPSIFD, &gps_offset), 1);

  // Visit main IFD 1 and record its offset.
  ASSERT_EQ(TIFFSetDirectory(rd.get(), 1), 1);
  EXPECT_EQ(TIFFLastDirectory(rd.get()), 1);
  main1_offset = TIFFCurrentDirOffset(rd.get());
  EXPECT_GT(main1_offset, 0ULL);

  // Jump via TIFFSetSubDirectory to main IFD 1 (dirn >= 1 in hash map),
  // main IFD 0 (dirn == 0 in hash map), actual SubIFD, and offset 0.
  EXPECT_EQ(TIFFSetSubDirectory(rd.get(), main1_offset), 1);
  EXPECT_EQ(TIFFCurrentDirectory(rd.get()), 1);
  const uint64_t main0_offset = [&] {
    EXPECT_EQ(TIFFSetDirectory(rd.get(), 0), 1);
    return TIFFCurrentDirOffset(rd.get());
  }();
  EXPECT_EQ(TIFFSetSubDirectory(rd.get(), main0_offset), 1);
  EXPECT_EQ(TIFFCurrentDirectory(rd.get()), 0);

  EXPECT_EQ(TIFFSetSubDirectory(rd.get(), subifd_offset), 1);
  uint32_t sub_w = 0;
  EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_IMAGEWIDTH, &sub_w), 1);
  EXPECT_EQ(sub_w, 2U);

  // Return from SubIFD back to main IFD 0 via tif_setdirectory_force_absolute.
  EXPECT_EQ(TIFFSetDirectory(rd.get(), 0), 1);
  uint32_t main_w = 0;
  EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_IMAGEWIDTH, &main_w), 1);
  EXPECT_EQ(main_w, 4U);

  // Read EXIF, GPS, and custom IFDs and verify values.
  ASSERT_EQ(TIFFReadEXIFDirectory(rd.get(), exif_offset), 1);
  float exp_time = 0.0f;
  uint8_t file_src = 0;
  EXPECT_EQ(TIFFGetField(rd.get(), EXIFTAG_EXPOSURETIME, &exp_time), 1);
  EXPECT_FLOAT_EQ(exp_time, 0.01f);
  EXPECT_EQ(TIFFGetField(rd.get(), EXIFTAG_FILESOURCE, &file_src), 1);
  EXPECT_EQ(file_src, 3);

  ASSERT_EQ(TIFFReadGPSDirectory(rd.get(), gps_offset), 1);
  const double* got_lat = nullptr;
  double got_alt = 0.0;
  EXPECT_EQ(TIFFGetField(rd.get(), GPSTAG_LATITUDE, &got_lat), 1);
  EXPECT_DOUBLE_EQ(got_lat[2], 19.25);
  EXPECT_EQ(TIFFGetField(rd.get(), GPSTAG_ALTITUDE, &got_alt), 1);
  EXPECT_DOUBLE_EQ(got_alt, 123.5);

  ASSERT_EQ(
      TIFFReadCustomDirectory(rd.get(), custom_offset, &kCustomDirFieldArray),
      1);
  uint16_t cshort = 0;
  const char* cascii = nullptr;
  EXPECT_EQ(TIFFGetField(rd.get(), kCustomIfdTagShort, &cshort), 1);
  EXPECT_EQ(cshort, 42);
  EXPECT_EQ(TIFFGetField(rd.get(), kCustomIfdTagAscii, &cascii), 1);
  EXPECT_STREQ(cascii, "CustomVal");

  // Returning from custom IFD to main IFD 0 exercises force_absolute again.
  ASSERT_EQ(TIFFSetDirectory(rd.get(), 0), 1);
  EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_IMAGEWIDTH, &main_w), 1);
  EXPECT_EQ(main_w, 4U);

  // Invalid sub-directory offset fails cleanly; 0 prepares fresh dir.
  EXPECT_EQ(TIFFSetSubDirectory(rd.get(), 0xFFFFFF00ULL), 0);
  EXPECT_EQ(TIFFSetSubDirectory(rd.get(), 0), 0);
}

TEST(TifDirTest, NavigationAndUnlinkClassicAndBigEndianAndBigTiff) {
  for (const char* write_mode : {"wl", "wb", "w8l", "w8b"}) {
    SCOPED_TRACE(write_mode);
    MemoryTiffStream stream;
    {
      UniqueTiff w = stream.Open(write_mode);
      ASSERT_NE(w, nullptr);
      // Unlinking on empty file (curdircount == 0) returns 0 cleanly.
      EXPECT_EQ(TIFFUnlinkDirectory(w.get(), 1), 0);

      for (uint32_t d = 0; d < 3; ++d) {
        WriteMinimalStripImage(w.get(), 4 + d, 2, static_cast<uint8_t>(d));
        ASSERT_EQ(TIFFWriteDirectory(w.get()), 1);
      }
    }

    // Read-only mapped ("r") and unmapped ("rm") navigation.
    stream.set_enable_mmap(true);
    for (const char* read_mode : {"r", "rm"}) {
      SCOPED_TRACE(read_mode);
      UniqueTiff rd = stream.Open(read_mode);
      ASSERT_NE(rd, nullptr);
      EXPECT_EQ(TIFFCurrentDirectory(rd.get()), 0);
      EXPECT_EQ(TIFFLastDirectory(rd.get()), 0);

      // Relative jump forward 0 -> 2 before hash map knows dir 2.
      ASSERT_EQ(TIFFSetDirectory(rd.get(), 2), 1);
      EXPECT_EQ(TIFFCurrentDirectory(rd.get()), 2);
      EXPECT_EQ(TIFFLastDirectory(rd.get()), 1);
      uint32_t w = 0;
      EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_IMAGEWIDTH, &w), 1);
      EXPECT_EQ(w, 6U);

      EXPECT_EQ(TIFFNumberOfDirectories(rd.get()), 3);
      // Hash-cached jump backward 2 -> 1 -> 0.
      ASSERT_EQ(TIFFSetDirectory(rd.get(), 1), 1);
      EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_IMAGEWIDTH, &w), 1);
      EXPECT_EQ(w, 5U);
      ASSERT_EQ(TIFFSetDirectory(rd.get(), 0), 1);
      EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_IMAGEWIDTH, &w), 1);
      EXPECT_EQ(w, 4U);

      EXPECT_EQ(TIFFSetDirectory(rd.get(), 99), 0);
      EXPECT_EQ(TIFFUnlinkDirectory(rd.get(), 1), 0);
    }

    // Header-only open ("rh") starts with tif_diroff == 0 -> SetDirectory(1).
    {
      UniqueTiff rh = stream.Open("rh");
      ASSERT_NE(rh, nullptr);
      EXPECT_EQ(TIFFSetDirectory(rh.get(), 1), 1);
      uint32_t w = 0;
      EXPECT_EQ(TIFFGetField(rh.get(), TIFFTAG_IMAGEWIDTH, &w), 1);
      EXPECT_EQ(w, 5U);
    }

    // Unlink directories in "r+" mode.
    {
      UniqueTiff rw = stream.Open("r+");
      ASSERT_NE(rw, nullptr);
      EXPECT_EQ(TIFFUnlinkDirectory(rw.get(), 0), 0);
      EXPECT_EQ(TIFFUnlinkDirectory(rw.get(), 5), 0);
      EXPECT_EQ(TIFFUnlinkDirectory(rw.get(), 4), 0);

      // Setup a raw write buffer before unlinking dir 2 to exercise rawdata
      // free.
      ASSERT_EQ(TIFFSetDirectory(rw.get(), 2), 1);
      ASSERT_EQ(TIFFWriteBufferSetup(rw.get(), nullptr, 64), 1);
      ASSERT_EQ(TIFFUnlinkDirectory(rw.get(), 2), 1);
      EXPECT_EQ(TIFFNumberOfDirectories(rw.get()), 2);

      // Simulate write failure during link patch.
      stream.FailWritesAfter(stream.write_calls());
      EXPECT_EQ(TIFFUnlinkDirectory(rw.get(), 1), 0);
      stream.ResetFaults();

      // Unlink first directory (dirn == 1).
      ASSERT_EQ(TIFFUnlinkDirectory(rw.get(), 1), 1);
      EXPECT_EQ(TIFFNumberOfDirectories(rw.get()), 1);
    }

    UniqueTiff verify = stream.Open("r");
    ASSERT_NE(verify, nullptr);
    EXPECT_EQ(TIFFNumberOfDirectories(verify.get()), 1);
    uint32_t remaining_w = 0;
    EXPECT_EQ(TIFFGetField(verify.get(), TIFFTAG_IMAGEWIDTH, &remaining_w), 1);
    EXPECT_EQ(remaining_w, 6U);
  }
}

TEST(TifDirTest, AdvanceDirectoryLoopsTruncationsAndDeferStrileFailures) {
  // 1. IFD loops (ClassicTIFF and BigTIFF, mapped and unmapped).
  for (bool big_tiff : {false, true}) {
    for (bool mapped : {false, true}) {
      SCOPED_TRACE(big_tiff ? "BigTIFF" : "ClassicTIFF");
      SCOPED_TRACE(mapped ? "mapped" : "unmapped");
      RawTiffBuilder builder(false, big_tiff);
      const std::array<uint8_t, 2> kPix = {0x77, 0x00};
      const uint64_t blob_off = builder.AppendBlob(kPix);
      const uint64_t off0 =
          builder.AddIfd(MakeMinimalClassicOrBigIfdEntries(builder, blob_off));
      builder.AddIfd(MakeMinimalClassicOrBigIfdEntries(builder, blob_off),
                     /*explicit_next_ifd_offset=*/off0);

      MemoryTiffStream stream(builder.Take());
      stream.set_enable_mmap(mapped);
      UniqueTiff tif = stream.Open(mapped ? "r" : "rm");
      ASSERT_NE(tif, nullptr);
      EXPECT_EQ(TIFFNumberOfDirectories(tif.get()), 2);
      EXPECT_EQ(TIFFSetDirectory(tif.get(), 2), 0);
    }
  }

  // 2. ClassicTIFF mapped & unmapped truncation errors in TIFFAdvanceDirectory.
  for (bool mapped : {false, true}) {
    SCOPED_TRACE(mapped ? "classic_mapped" : "classic_unmapped");
    // Truncated at dircount.
    {
      RawTiffBuilder builder(false, false);
      const std::array<uint8_t, 2> kPix = {0x11, 0x00};
      const uint64_t blob_off = builder.AppendBlob(kPix);
      builder.AddIfd(MakeMinimalClassicOrBigIfdEntries(builder, blob_off));
      const uint64_t bad_next = builder.bytes().size() + 100;
      RawTiffBuilder bad(false, false);
      bad.PatchHeaderFirstIfdOffset(bad_next);

      MemoryTiffStream stream(bad.Take());
      stream.set_enable_mmap(mapped);
      UniqueTiff tif = stream.Open(mapped ? "rh" : "rmh");
      ASSERT_NE(tif, nullptr);
      EXPECT_EQ(TIFFNumberOfDirectories(tif.get()), 0);
    }
    // Truncated inside IFD before nextdir32.
    {
      RawTiffBuilder builder(false, false);
      const std::array<uint8_t, 2> kPix = {0x11, 0x00};
      const uint64_t blob_off = builder.AppendBlob(kPix);
      const uint64_t off0 =
          builder.AddIfd(MakeMinimalClassicOrBigIfdEntries(builder, blob_off));
      // Truncate right after dircount + 1 entry so nextdir32 falls past EOF.
      std::vector<uint8_t> bytes = builder.Take();
      bytes.resize(static_cast<size_t>(off0) + 14);

      MemoryTiffStream stream(std::move(bytes));
      stream.set_enable_mmap(mapped);
      UniqueTiff tif = stream.Open(mapped ? "rh" : "rmh");
      ASSERT_NE(tif, nullptr);
      EXPECT_EQ(TIFFNumberOfDirectories(tif.get()), 0);
    }
  }

  // 3. BigTIFF mapped & unmapped errors in TIFFAdvanceDirectory.
  for (bool mapped : {false, true}) {
    SCOPED_TRACE(mapped ? "bigtiff_mapped" : "bigtiff_unmapped");
    // Truncated at dircount64.
    {
      RawTiffBuilder bad(false, true);
      bad.PatchHeaderFirstIfdOffset(bad.bytes().size() + 100);
      MemoryTiffStream stream(bad.Take());
      stream.set_enable_mmap(mapped);
      UniqueTiff tif = stream.Open(mapped ? "rh" : "rmh");
      ASSERT_NE(tif, nullptr);
      EXPECT_EQ(TIFFNumberOfDirectories(tif.get()), 0);
    }
    // Excessive dircount64 > 0xFFFF.
    {
      RawTiffBuilder bad(false, true);
      const uint64_t ifd_off = bad.bytes().size();
      bad.PatchHeaderFirstIfdOffset(ifd_off);
      // Append 8-byte dircount64 = 0x10000 (little-endian).
      std::vector<uint8_t> bytes = bad.Take();
      bytes.push_back(0x00);
      bytes.push_back(0x00);
      bytes.push_back(0x01);
      bytes.insert(bytes.end(), 5, 0x00);

      MemoryTiffStream stream(std::move(bytes));
      stream.set_enable_mmap(mapped);
      UniqueTiff tif = stream.Open(mapped ? "rh" : "rmh");
      ASSERT_NE(tif, nullptr);
      EXPECT_EQ(TIFFNumberOfDirectories(tif.get()), 0);
    }
    // Truncated inside BigTIFF IFD before nextdiroff.
    {
      RawTiffBuilder builder(false, true);
      const std::array<uint8_t, 2> kPix = {0x11, 0x00};
      const uint64_t blob_off = builder.AppendBlob(kPix);
      const uint64_t off0 =
          builder.AddIfd(MakeMinimalClassicOrBigIfdEntries(builder, blob_off));
      std::vector<uint8_t> bytes = builder.Take();
      bytes.resize(static_cast<size_t>(off0) + 28);

      MemoryTiffStream stream(std::move(bytes));
      stream.set_enable_mmap(mapped);
      UniqueTiff tif = stream.Open(mapped ? "rh" : "rmh");
      ASSERT_NE(tif, nullptr);
      EXPECT_EQ(TIFFNumberOfDirectories(tif.get()), 0);
    }
  }

  // 4. BigTIFF mapped offset overflow (poff > TIFF_TMSIZE_T_MAX - 8) &
  //    corrupt IFD 0 via TIFFSetDirectory(0).
  {
    RawTiffBuilder bad(false, true);
    bad.PatchHeaderFirstIfdOffset(static_cast<uint64_t>(TIFF_TMSIZE_T_MAX) - 4);
    MemoryTiffStream stream(bad.Take());
    stream.set_enable_mmap(true);
    UniqueTiff tif = stream.Open("rh");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFNumberOfDirectories(tif.get()), 0);
  }
  {
    ScopedTiffErrorHandlers silent(nullptr, nullptr);
    RawTiffBuilder empty_ifd(false, false);
    empty_ifd.AddIfd({});
    MemoryTiffStream stream(empty_ifd.Take());
    UniqueTiff tif = stream.Open("rh");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFSetDirectory(tif.get(), 0), 0);
    EXPECT_EQ(TIFFCurrentDirectory(tif.get()), TIFF_NON_EXISTENT_DIR_NUMBER);
  }

  // 5. Deferred strile loading failure makes TIFFGetField for
  //    STRIPOFFSETS/STRIPBYTECOUNTS return 0.
  {
    MemoryTiffStream stream;
    {
      UniqueTiff w = stream.Open("w");
      ASSERT_NE(w, nullptr);
      ASSERT_EQ(TIFFSetField(w.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
      ASSERT_EQ(TIFFSetField(w.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}), 1);
      ASSERT_EQ(TIFFSetField(w.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
      ASSERT_EQ(TIFFSetField(w.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      ASSERT_EQ(TIFFSetField(w.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{1}), 1);
      std::array<uint8_t, 4> row = {1, 2, 3, 4};
      for (uint32_t r = 0; r < 4; ++r) {
        ASSERT_EQ(TIFFWriteScanline(w.get(), row.data(), r, 0), 1);
      }
      ASSERT_EQ(TIFFWriteDirectory(w.get()), 1);
    }
    UniqueTiff rd = stream.Open("rmD");
    ASSERT_NE(rd, nullptr);
    stream.FailReadsAfter(stream.read_calls());
    const uint64_t* ptr = nullptr;
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_STRIPOFFSETS, &ptr), 0);
    EXPECT_EQ(TIFFGetField(rd.get(), TIFFTAG_STRIPBYTECOUNTS, &ptr), 0);
  }
}

}  // namespace
}  // namespace libtiff
