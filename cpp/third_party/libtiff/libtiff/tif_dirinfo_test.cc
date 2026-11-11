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

// Clean-room unit tests for libtiff directory tag metadata and lookup
// routines (libtiff/tif_dirinfo.c).

#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/temp_file.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tif_dir.h"
#include "third_party/tiff/libtiff/tif_jxl.h"
#include "third_party/tiff/libtiff/tiff.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

using ::testing::HasSubstr;

constexpr uint32_t kTestCustomTagBase = 64000;

int DummyCodecInit(TIFF* /*tif*/, int /*scheme*/) { return 1; }

UniqueTiff OpenMemoryWithCumulatedBudget(MemoryTiffStream& stream,
                                         const char* mode,
                                         tmsize_t max_cumulated = 1 << 24) {
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  stream.diagnostics().Install(opts.get());
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), max_cumulated);
  return stream.Open(mode, opts.get());
}

void WriteSingleGrayPixelImage(TIFF* tif, uint8_t pixel) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, uint32_t{1}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, uint32_t{1}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, uint32_t{1}), 1);
  uint8_t mutable_pixel = pixel;
  ASSERT_EQ(TIFFWriteScanline(tif, &mutable_pixel, 0, 0), 1);
}

TEST(TifDirInfoTest, DataWidthAllTypes) {
  struct DataWidthCase {
    TIFFDataType type;
    int expected_width;
  };
  constexpr auto kCases = std::to_array<DataWidthCase>({
      {.type = TIFF_NOTYPE, .expected_width = 1},
      {.type = TIFF_BYTE, .expected_width = 1},
      {.type = TIFF_ASCII, .expected_width = 1},
      {.type = TIFF_SBYTE, .expected_width = 1},
      {.type = TIFF_UNDEFINED, .expected_width = 1},
      {.type = TIFF_SHORT, .expected_width = 2},
      {.type = TIFF_SSHORT, .expected_width = 2},
      {.type = TIFF_LONG, .expected_width = 4},
      {.type = TIFF_SLONG, .expected_width = 4},
      {.type = TIFF_FLOAT, .expected_width = 4},
      {.type = TIFF_IFD, .expected_width = 4},
      {.type = TIFF_RATIONAL, .expected_width = 8},
      {.type = TIFF_SRATIONAL, .expected_width = 8},
      {.type = TIFF_DOUBLE, .expected_width = 8},
      {.type = TIFF_LONG8, .expected_width = 8},
      {.type = TIFF_SLONG8, .expected_width = 8},
      {.type = TIFF_IFD8, .expected_width = 8},
      {.type = static_cast<TIFFDataType>(14), .expected_width = 0},
      {.type = static_cast<TIFFDataType>(15), .expected_width = 0},
      {.type = static_cast<TIFFDataType>(19), .expected_width = 0},
      {.type = static_cast<TIFFDataType>(25), .expected_width = 0},
      {.type = static_cast<TIFFDataType>(31), .expected_width = 0},
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(static_cast<int>(c.type));
    EXPECT_EQ(TIFFDataWidth(c.type), c.expected_width);
  }
}

TEST(TifDirInfoTest, FieldSetGetSizeAndCountSizeAllEnumValues) {
  EXPECT_EQ(TIFFFieldSetGetSize(nullptr), 0);
  EXPECT_EQ(TIFFFieldSetGetCountSize(nullptr), 0);

  struct SetGetCase {
    TIFFSetGetFieldType type;
    int expected_size;
    int expected_count_size;
  };
  constexpr auto kCases = std::to_array<SetGetCase>({
      {.type = TIFF_SETGET_UNDEFINED,
       .expected_size = 1,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_ASCII, .expected_size = 1, .expected_count_size = 0},
      {.type = TIFF_SETGET_UINT8, .expected_size = 1, .expected_count_size = 0},
      {.type = TIFF_SETGET_SINT8, .expected_size = 1, .expected_count_size = 0},
      {.type = TIFF_SETGET_UINT16,
       .expected_size = 2,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_SINT16,
       .expected_size = 2,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_UINT32,
       .expected_size = 4,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_SINT32,
       .expected_size = 4,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_UINT64,
       .expected_size = 8,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_SINT64,
       .expected_size = 8,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_FLOAT, .expected_size = 4, .expected_count_size = 0},
      {.type = TIFF_SETGET_DOUBLE,
       .expected_size = 8,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_IFD8, .expected_size = 8, .expected_count_size = 0},
      {.type = TIFF_SETGET_INT, .expected_size = 4, .expected_count_size = 0},
      {.type = TIFF_SETGET_UINT16_PAIR,
       .expected_size = 4,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_C0_ASCII,
       .expected_size = 1,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_C0_UINT8,
       .expected_size = 1,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_C0_SINT8,
       .expected_size = 1,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_C0_UINT16,
       .expected_size = 2,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_C0_SINT16,
       .expected_size = 2,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_C0_UINT32,
       .expected_size = 4,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_C0_SINT32,
       .expected_size = 4,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_C0_UINT64,
       .expected_size = 8,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_C0_SINT64,
       .expected_size = 8,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_C0_FLOAT,
       .expected_size = 4,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_C0_DOUBLE,
       .expected_size = 8,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_C0_IFD8,
       .expected_size = 8,
       .expected_count_size = 0},
      {.type = TIFF_SETGET_C16_ASCII,
       .expected_size = 1,
       .expected_count_size = 2},
      {.type = TIFF_SETGET_C16_UINT8,
       .expected_size = 1,
       .expected_count_size = 2},
      {.type = TIFF_SETGET_C16_SINT8,
       .expected_size = 1,
       .expected_count_size = 2},
      {.type = TIFF_SETGET_C16_UINT16,
       .expected_size = 2,
       .expected_count_size = 2},
      {.type = TIFF_SETGET_C16_SINT16,
       .expected_size = 2,
       .expected_count_size = 2},
      {.type = TIFF_SETGET_C16_UINT32,
       .expected_size = 4,
       .expected_count_size = 2},
      {.type = TIFF_SETGET_C16_SINT32,
       .expected_size = 4,
       .expected_count_size = 2},
      {.type = TIFF_SETGET_C16_UINT64,
       .expected_size = 8,
       .expected_count_size = 2},
      {.type = TIFF_SETGET_C16_SINT64,
       .expected_size = 8,
       .expected_count_size = 2},
      {.type = TIFF_SETGET_C16_FLOAT,
       .expected_size = 4,
       .expected_count_size = 2},
      {.type = TIFF_SETGET_C16_DOUBLE,
       .expected_size = 8,
       .expected_count_size = 2},
      {.type = TIFF_SETGET_C16_IFD8,
       .expected_size = 8,
       .expected_count_size = 2},
      {.type = TIFF_SETGET_C32_ASCII,
       .expected_size = 1,
       .expected_count_size = 4},
      {.type = TIFF_SETGET_C32_UINT8,
       .expected_size = 1,
       .expected_count_size = 4},
      {.type = TIFF_SETGET_C32_SINT8,
       .expected_size = 1,
       .expected_count_size = 4},
      {.type = TIFF_SETGET_C32_UINT16,
       .expected_size = 2,
       .expected_count_size = 4},
      {.type = TIFF_SETGET_C32_SINT16,
       .expected_size = 2,
       .expected_count_size = 4},
      {.type = TIFF_SETGET_C32_UINT32,
       .expected_size = 4,
       .expected_count_size = 4},
      {.type = TIFF_SETGET_C32_SINT32,
       .expected_size = 4,
       .expected_count_size = 4},
      {.type = TIFF_SETGET_C32_UINT64,
       .expected_size = 8,
       .expected_count_size = 4},
      {.type = TIFF_SETGET_C32_SINT64,
       .expected_size = 8,
       .expected_count_size = 4},
      {.type = TIFF_SETGET_C32_FLOAT,
       .expected_size = 4,
       .expected_count_size = 4},
      {.type = TIFF_SETGET_C32_DOUBLE,
       .expected_size = 8,
       .expected_count_size = 4},
      {.type = TIFF_SETGET_C32_IFD8,
       .expected_size = 8,
       .expected_count_size = 4},
      {.type = TIFF_SETGET_OTHER, .expected_size = 1, .expected_count_size = 0},
      {.type = static_cast<TIFFSetGetFieldType>(52),
       .expected_size = 0,
       .expected_count_size = 0},
      {.type = static_cast<TIFFSetGetFieldType>(63),
       .expected_size = 0,
       .expected_count_size = 0},
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(static_cast<int>(c.type));
    TIFFField f{};
    f.set_get_field_type = c.type;
    EXPECT_EQ(TIFFFieldSetGetSize(&f), c.expected_size);
    EXPECT_EQ(TIFFFieldSetGetCountSize(&f), c.expected_count_size);
  }
}

TEST(TifDirInfoTest, BuiltinFieldMetadataAndAccessors) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  struct FieldExpectation {
    uint32_t tag;
    std::string_view name;
    TIFFDataType type;
    int pass_count;
    int read_count;
    int write_count;
    int set_get_size;
    int set_get_count_size;
  };
  constexpr auto kBuiltinChecks = std::to_array<FieldExpectation>({
      {.tag = TIFFTAG_IMAGEWIDTH,
       .name = "ImageWidth",
       .type = TIFF_LONG,
       .pass_count = 0,
       .read_count = 1,
       .write_count = 1,
       .set_get_size = 4,
       .set_get_count_size = 0},
      {.tag = TIFFTAG_COMPRESSION,
       .name = "Compression",
       .type = TIFF_SHORT,
       .pass_count = 0,
       .read_count = 1,
       .write_count = 1,
       .set_get_size = 2,
       .set_get_count_size = 0},
      {.tag = TIFFTAG_FAXPROFILE,
       .name = "FaxProfile",
       .type = TIFF_BYTE,
       .pass_count = 0,
       .read_count = 1,
       .write_count = 1,
       .set_get_size = 1,
       .set_get_count_size = 0},
      {.tag = TIFFTAG_DOCUMENTNAME,
       .name = "DocumentName",
       .type = TIFF_ASCII,
       .pass_count = 0,
       .read_count = TIFF_VARIABLE,
       .write_count = TIFF_VARIABLE,
       .set_get_size = 1,
       .set_get_count_size = 0},
      {.tag = TIFFTAG_MINSAMPLEVALUE,
       .name = "MinSampleValue",
       .type = TIFF_SHORT,
       .pass_count = 0,
       .read_count = TIFF_SPP,
       .write_count = TIFF_VARIABLE,
       .set_get_size = 2,
       .set_get_count_size = 0},
      {.tag = TIFFTAG_XRESOLUTION,
       .name = "XResolution",
       .type = TIFF_RATIONAL,
       .pass_count = 0,
       .read_count = 1,
       .write_count = 1,
       .set_get_size = 4,
       .set_get_count_size = 0},
      {.tag = TIFFTAG_PAGENUMBER,
       .name = "PageNumber",
       .type = TIFF_SHORT,
       .pass_count = 0,
       .read_count = 2,
       .write_count = 2,
       .set_get_size = 4,
       .set_get_count_size = 0},
      {.tag = TIFFTAG_COLORMAP,
       .name = "ColorMap",
       .type = TIFF_SHORT,
       .pass_count = 0,
       .read_count = TIFF_VARIABLE,
       .write_count = TIFF_VARIABLE,
       .set_get_size = 1,
       .set_get_count_size = 0},
      {.tag = TIFFTAG_SUBIFD,
       .name = "SubIFD",
       .type = TIFF_IFD8,
       .pass_count = 1,
       .read_count = TIFF_VARIABLE,
       .write_count = TIFF_VARIABLE,
       .set_get_size = 8,
       .set_get_count_size = 2},
      {.tag = TIFFTAG_INKNAMES,
       .name = "InkNames",
       .type = TIFF_ASCII,
       .pass_count = 1,
       .read_count = TIFF_VARIABLE,
       .write_count = TIFF_VARIABLE,
       .set_get_size = 1,
       .set_get_count_size = 2},
      {.tag = TIFFTAG_EXTRASAMPLES,
       .name = "ExtraSamples",
       .type = TIFF_SHORT,
       .pass_count = 1,
       .read_count = TIFF_VARIABLE,
       .write_count = TIFF_VARIABLE,
       .set_get_size = 2,
       .set_get_count_size = 2},
      {.tag = TIFFTAG_XMLPACKET,
       .name = "XMLPacket",
       .type = TIFF_BYTE,
       .pass_count = 1,
       .read_count = TIFF_VARIABLE2,
       .write_count = TIFF_VARIABLE2,
       .set_get_size = 1,
       .set_get_count_size = 4},
      {.tag = TIFFTAG_STONITS,
       .name = "StoNits",
       .type = TIFF_DOUBLE,
       .pass_count = 0,
       .read_count = 1,
       .write_count = 1,
       .set_get_size = 8,
       .set_get_count_size = 0},
      {.tag = TIFFTAG_PERSAMPLE,
       .name = "PerSample",
       .type = TIFF_SHORT,
       .pass_count = 0,
       .read_count = 0,
       .write_count = 0,
       .set_get_size = 1,
       .set_get_count_size = 0},
  });

  for (const auto& exp : kBuiltinChecks) {
    SCOPED_TRACE(exp.name);
    const TIFFField* fip = TIFFFindField(tif.get(), exp.tag, exp.type);
    ASSERT_NE(fip, nullptr);
    EXPECT_EQ(TIFFFieldTag(fip), exp.tag);
    ASSERT_NE(TIFFFieldName(fip), nullptr);
    EXPECT_EQ(std::string_view(TIFFFieldName(fip)), exp.name);
    EXPECT_EQ(TIFFFieldDataType(fip), exp.type);
    EXPECT_EQ(TIFFFieldPassCount(fip), exp.pass_count);
    EXPECT_EQ(TIFFFieldReadCount(fip), exp.read_count);
    EXPECT_EQ(TIFFFieldWriteCount(fip), exp.write_count);
    EXPECT_EQ(TIFFFieldSetGetSize(fip), exp.set_get_size);
    EXPECT_EQ(TIFFFieldSetGetCountSize(fip), exp.set_get_count_size);
    EXPECT_EQ(TIFFFieldIsAnonymous(fip), 0);
  }

  const TIFFField* subifd = TIFFFindField(tif.get(), TIFFTAG_SUBIFD, TIFF_ANY);
  ASSERT_NE(subifd, nullptr);
  EXPECT_EQ(subifd->field_subfields, _TIFFGetFields());

  const TIFFField* exif = TIFFFindField(tif.get(), TIFFTAG_EXIFIFD, TIFF_ANY);
  ASSERT_NE(exif, nullptr);
  EXPECT_EQ(exif->field_subfields, _TIFFGetExifFields());

  const TIFFField* gps = TIFFFindField(tif.get(), TIFFTAG_GPSIFD, TIFF_ANY);
  ASSERT_NE(gps, nullptr);
  EXPECT_EQ(gps->field_subfields, _TIFFGetGpsFields());
  EXPECT_EQ(stream.diagnostics().warning_count(), 0);
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
}

TEST(TifDirInfoTest, BuiltinFieldArraysSortedAfterSetup) {
  const TIFFFieldArray* tiff_arr = _TIFFGetFields();
  const TIFFFieldArray* exif_arr = _TIFFGetExifFields();
  const TIFFFieldArray* gps_arr = _TIFFGetGpsFields();
  ASSERT_NE(tiff_arr, nullptr);
  ASSERT_NE(exif_arr, nullptr);
  ASSERT_NE(gps_arr, nullptr);
  EXPECT_EQ(tiff_arr->type, tfiatImage);
  EXPECT_EQ(exif_arr->type, tfiatExif);
  EXPECT_EQ(gps_arr->type, tfiatGps);
  EXPECT_GT(tiff_arr->count, 0U);
  EXPECT_GT(exif_arr->count, 0U);
  EXPECT_GT(gps_arr->count, 0U);

  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  auto verify_sorted_and_findable = [&](const TIFFFieldArray* arr) {
    ASSERT_EQ(tif->tif_nfields, arr->count);
    for (size_t i = 1; i < tif->tif_nfields; ++i) {
      EXPECT_LT(tif->tif_fields[i - 1]->field_tag,
                tif->tif_fields[i]->field_tag);
    }
    for (uint32_t i = 0; i < arr->count; ++i) {
      const TIFFField* expected = &arr->fields[i];
      EXPECT_EQ(TIFFFindField(tif.get(), expected->field_tag, TIFF_ANY),
                expected);
      EXPECT_EQ(TIFFFieldWithName(tif.get(), expected->field_name), expected);
    }
  };

  verify_sorted_and_findable(tiff_arr);
  ASSERT_EQ(TIFFCreateEXIFDirectory(tif.get()), 0);
  verify_sorted_and_findable(exif_arr);
  ASSERT_EQ(TIFFCreateGPSDirectory(tif.get()), 0);
  verify_sorted_and_findable(gps_arr);
  EXPECT_EQ(stream.diagnostics().warning_count(), 0);
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
}

TEST(TifDirInfoTest, FindFieldByTagAndNameCachingAndErrors) {
  MemoryTiffStream stream;
  UniqueTiff tif = OpenMemoryWithCumulatedBudget(stream, "w");
  ASSERT_NE(tif, nullptr);

  // Tag lookup cache hits (TIFF_ANY and exact dt) and type-mismatch miss.
  const TIFFField* width =
      TIFFFindField(tif.get(), TIFFTAG_IMAGEWIDTH, TIFF_ANY);
  ASSERT_NE(width, nullptr);
  EXPECT_EQ(tif->tif_foundfield, width);
  EXPECT_EQ(TIFFFindField(tif.get(), TIFFTAG_IMAGEWIDTH, TIFF_ANY), width);
  EXPECT_EQ(TIFFFindField(tif.get(), TIFFTAG_IMAGEWIDTH, TIFF_LONG), width);
  EXPECT_EQ(TIFFFindField(tif.get(), TIFFTAG_IMAGEWIDTH, TIFF_SHORT), nullptr);
  EXPECT_EQ(tif->tif_foundfield, nullptr);

  // TIFFTAG_SMINSAMPLEVALUE is defined with field_type == TIFF_ANY.
  const TIFFField* smin =
      TIFFFindField(tif.get(), TIFFTAG_SMINSAMPLEVALUE, TIFF_ANY);
  ASSERT_NE(smin, nullptr);
  EXPECT_EQ(TIFFFieldDataType(smin), TIFF_ANY);
  EXPECT_EQ(TIFFFindField(tif.get(), TIFFTAG_SMINSAMPLEVALUE, TIFF_DOUBLE),
            nullptr);

  // Cross-lookup cache hit: populate tif_foundfield by tag, then query by name.
  const TIFFField* artist = TIFFFieldWithTag(tif.get(), TIFFTAG_ARTIST);
  ASSERT_NE(artist, nullptr);
  EXPECT_EQ(TIFFFieldWithName(tif.get(), "Artist"), artist);
  EXPECT_EQ(stream.diagnostics().warning_count(), 0);

  // Linear name search cache miss -> hit.
  const TIFFField* stonits = TIFFFieldWithName(tif.get(), "StoNits");
  ASSERT_NE(stonits, nullptr);
  EXPECT_EQ(TIFFFieldTag(stonits), TIFFTAG_STONITS);
  EXPECT_EQ(TIFFFieldWithName(tif.get(), "StoNits"), stonits);

  // Unknown tag / name emit warnings via TIFFWarningExtR.
  EXPECT_EQ(TIFFFieldWithTag(tif.get(), 60001), nullptr);
  EXPECT_EQ(TIFFFieldWithName(tif.get(), "NonExistentFieldName"), nullptr);
  const auto warnings = stream.diagnostics().warnings();
  ASSERT_EQ(warnings.size(), 2U);
  EXPECT_EQ(warnings[0].module, "TIFFFieldWithTag");
  EXPECT_THAT(warnings[0].message, HasSubstr("unknown tag"));
  EXPECT_EQ(warnings[1].module, "TIFFFieldWithName");
  EXPECT_THAT(warnings[1].message, HasSubstr("NonExistentFieldName"));

  // Force tif->tif_fields == nullptr via failed _TIFFSetupFields allocation.
  {
    ScopedSingleMemAllocLimit limit(tif.get(), 1);
    _TIFFSetupFields(tif.get(), _TIFFGetFields());
  }
  EXPECT_EQ(tif->tif_fields, nullptr);
  EXPECT_EQ(tif->tif_nfields, 0U);
  EXPECT_EQ(TIFFFindField(tif.get(), TIFFTAG_IMAGEWIDTH, TIFF_ANY), nullptr);
  EXPECT_EQ(TIFFFieldWithName(tif.get(), "ImageWidth"), nullptr);

  // Restore valid fields before closing.
  _TIFFSetupFields(tif.get(), _TIFFGetFields());
  EXPECT_NE(TIFFFindField(tif.get(), TIFFTAG_IMAGEWIDTH, TIFF_ANY), nullptr);
}

TEST(TifDirInfoTest, ExifGpsAndSubIfdDirectoriesRoundTrip) {
  MemoryTiffStream stream;
  uint64_t exif_offset = 0;
  uint64_t gps_offset = 0;
  uint64_t subifd_offset = 0;
  {
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);

    WriteSingleGrayPixelImage(writer.get(), 42);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_EXIFIFD, uint64_t{0}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_GPSIFD, uint64_t{0}), 1);
    uint64_t subifd_placeholder = 0;
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SUBIFD, uint16_t{1},
                           &subifd_placeholder),
              1);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);

    // SubIFD automatically follows because TIFFTAG_SUBIFD was set.
    WriteSingleGrayPixelImage(writer.get(), 99);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEDESCRIPTION, "subifd"),
              1);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);

    // EXIF custom directory.
    ASSERT_EQ(TIFFCreateEXIFDirectory(writer.get()), 0);
    EXPECT_NE(TIFFFindField(writer.get(), EXIFTAG_EXPOSURETIME, TIFF_RATIONAL),
              nullptr);
    EXPECT_EQ(TIFFFindField(writer.get(), TIFFTAG_IMAGEWIDTH, TIFF_ANY),
              nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), EXIFTAG_EXPOSURETIME, 0.25), 1);
    constexpr uint8_t kExifVer[4] = {'0', '2', '3', '2'};
    ASSERT_EQ(TIFFSetField(writer.get(), EXIFTAG_EXIFVERSION, kExifVer), 1);
    constexpr uint16_t kIso = 400;
    ASSERT_EQ(
        TIFFSetField(writer.get(), EXIFTAG_ISOSPEEDRATINGS, uint16_t{1}, &kIso),
        1);
    ASSERT_EQ(TIFFWriteCustomDirectory(writer.get(), &exif_offset), 1);
    ASSERT_GT(exif_offset, 0U);

    // GPS custom directory (includes double-precision Rational2Double tags).
    ASSERT_EQ(TIFFCreateGPSDirectory(writer.get()), 0);
    const TIFFField* lat_f = TIFFFieldWithName(writer.get(), "Latitude");
    ASSERT_NE(lat_f, nullptr);
    EXPECT_EQ(TIFFFieldSetGetSize(lat_f), 8);
    const TIFFField* alt_f = TIFFFieldWithName(writer.get(), "Altitude");
    ASSERT_NE(alt_f, nullptr);
    EXPECT_EQ(TIFFFieldSetGetSize(alt_f), 8);
    constexpr uint8_t kGpsVer[4] = {2, 3, 0, 0};
    ASSERT_EQ(TIFFSetField(writer.get(), GPSTAG_VERSIONID, kGpsVer), 1);
    constexpr double kLat[3] = {37.0, 25.0, 19.5};
    ASSERT_EQ(TIFFSetField(writer.get(), GPSTAG_LATITUDE, kLat), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), GPSTAG_ALTITUDE, 123.5), 1);
    ASSERT_EQ(TIFFWriteCustomDirectory(writer.get(), &gps_offset), 1);
    ASSERT_GT(gps_offset, 0U);

    // Patch main IFD's EXIF and GPS offsets.
    ASSERT_EQ(TIFFSetDirectory(writer.get(), 0), 1);
    uint16_t n_sub = 0;
    uint64_t* sub_offsets = nullptr;
    ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_SUBIFD, &n_sub, &sub_offsets),
              1);
    ASSERT_EQ(n_sub, 1);
    subifd_offset = sub_offsets[0];
    ASSERT_GT(subifd_offset, 0U);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_EXIFIFD, exif_offset), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_GPSIFD, gps_offset), 1);
    ASSERT_EQ(TIFFRewriteDirectory(writer.get()), 1);
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);

  uint64_t read_exif_off = 0;
  uint64_t read_gps_off = 0;
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_EXIFIFD, &read_exif_off), 1);
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_GPSIFD, &read_gps_off), 1);
  EXPECT_EQ(read_exif_off, exif_offset);
  EXPECT_EQ(read_gps_off, gps_offset);

  ASSERT_EQ(TIFFReadEXIFDirectory(reader.get(), read_exif_off), 1);
  float exposure = 0.0f;
  ASSERT_EQ(TIFFGetField(reader.get(), EXIFTAG_EXPOSURETIME, &exposure), 1);
  EXPECT_FLOAT_EQ(exposure, 0.25f);
  uint16_t iso_count = 0;
  const uint16_t* iso_vals = nullptr;
  ASSERT_EQ(TIFFGetField(reader.get(), EXIFTAG_ISOSPEEDRATINGS, &iso_count,
                         &iso_vals),
            1);
  ASSERT_EQ(iso_count, 1);
  EXPECT_EQ(iso_vals[0], 400);

  ASSERT_EQ(TIFFReadGPSDirectory(reader.get(), read_gps_off), 1);
  const double* lat_out = nullptr;
  double alt_out = 0.0;
  ASSERT_EQ(TIFFGetField(reader.get(), GPSTAG_LATITUDE, &lat_out), 1);
  EXPECT_DOUBLE_EQ(lat_out[0], 37.0);
  EXPECT_DOUBLE_EQ(lat_out[1], 25.0);
  EXPECT_DOUBLE_EQ(lat_out[2], 19.5);
  ASSERT_EQ(TIFFGetField(reader.get(), GPSTAG_ALTITUDE, &alt_out), 1);
  EXPECT_DOUBLE_EQ(alt_out, 123.5);

  ASSERT_EQ(TIFFSetSubDirectory(reader.get(), subifd_offset), 1);
  const char* desc = nullptr;
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGEDESCRIPTION, &desc), 1);
  EXPECT_STREQ(desc, "subifd");
  EXPECT_NE(TIFFFieldWithName(reader.get(), "ImageWidth"), nullptr);
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
}

TEST(TifDirInfoTest, AnonFieldCreationAllTypesAndSetupFieldsCleanup) {
  MemoryTiffStream stream;
  UniqueTiff tif = OpenMemoryWithCumulatedBudget(stream, "w");
  ASSERT_NE(tif, nullptr);

  struct AnonCase {
    uint32_t tag;
    TIFFDataType dt;
    int expected_size;
    int expected_count_size;
  };
  constexpr auto kCases = std::to_array<AnonCase>({
      {.tag = kTestCustomTagBase + 0,
       .dt = TIFF_NOTYPE,
       .expected_size = 1,
       .expected_count_size = 0},
      {.tag = kTestCustomTagBase + 1,
       .dt = TIFF_BYTE,
       .expected_size = 1,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 2,
       .dt = TIFF_UNDEFINED,
       .expected_size = 1,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 3,
       .dt = TIFF_ASCII,
       .expected_size = 1,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 4,
       .dt = TIFF_SHORT,
       .expected_size = 2,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 5,
       .dt = TIFF_LONG,
       .expected_size = 4,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 6,
       .dt = TIFF_RATIONAL,
       .expected_size = 4,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 7,
       .dt = TIFF_SRATIONAL,
       .expected_size = 4,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 8,
       .dt = TIFF_FLOAT,
       .expected_size = 4,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 9,
       .dt = TIFF_SBYTE,
       .expected_size = 1,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 10,
       .dt = TIFF_SSHORT,
       .expected_size = 2,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 11,
       .dt = TIFF_SLONG,
       .expected_size = 4,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 12,
       .dt = TIFF_DOUBLE,
       .expected_size = 8,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 13,
       .dt = TIFF_IFD,
       .expected_size = 8,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 14,
       .dt = TIFF_IFD8,
       .expected_size = 8,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 15,
       .dt = TIFF_LONG8,
       .expected_size = 8,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 16,
       .dt = TIFF_SLONG8,
       .expected_size = 8,
       .expected_count_size = 4},
      {.tag = kTestCustomTagBase + 17,
       .dt = static_cast<TIFFDataType>(19),
       .expected_size = 1,
       .expected_count_size = 0},
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.tag);
    const TIFFField* fip = _TIFFFindOrRegisterField(tif.get(), c.tag, c.dt);
    ASSERT_NE(fip, nullptr);
    EXPECT_EQ(TIFFFieldTag(fip), c.tag);
    EXPECT_EQ(TIFFFieldDataType(fip), c.dt);
    EXPECT_EQ(TIFFFieldIsAnonymous(fip), 1);
    EXPECT_EQ(TIFFFieldPassCount(fip), 1);
    EXPECT_EQ(TIFFFieldReadCount(fip), TIFF_VARIABLE2);
    EXPECT_EQ(TIFFFieldWriteCount(fip), TIFF_VARIABLE2);
    EXPECT_EQ(TIFFFieldSetGetSize(fip), c.expected_size);
    EXPECT_EQ(TIFFFieldSetGetCountSize(fip), c.expected_count_size);
    EXPECT_EQ(TIFFFieldName(fip), absl::StrFormat("Tag %u", c.tag));
    // Subsequent call finds existing anonymous field without reallocating.
    EXPECT_EQ(_TIFFFindOrRegisterField(tif.get(), c.tag, c.dt), fip);
  }

  // Re-running _TIFFSetupFields frees all registered anonymous fields cleanly.
  _TIFFSetupFields(tif.get(), _TIFFGetFields());
  EXPECT_EQ(TIFFFindField(tif.get(), kTestCustomTagBase + 5, TIFF_ANY),
            nullptr);
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
}

TEST(TifDirInfoTest, ReadCraftedUnknownTagsAcrossEndianAndBigTiff) {
  for (bool big_endian : {false, true}) {
    for (bool big_tiff : {false, true}) {
      SCOPED_TRACE(big_endian ? "BE" : "LE");
      SCOPED_TRACE(big_tiff ? "BigTIFF" : "ClassicTIFF");

      RawTiffBuilder builder(big_endian, big_tiff);
      constexpr uint8_t kPixel = 0x5A;
      const uint64_t strip_off =
          builder.AppendBlob(absl::MakeConstSpan(&kPixel, 1));
      std::vector<RawIfdEntry> entries = {
          builder.MakeLong(TIFFTAG_IMAGEWIDTH, 1),
          builder.MakeLong(TIFFTAG_IMAGELENGTH, 1),
          builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
          builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
          builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
          builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                           static_cast<uint32_t>(strip_off)),
          builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
          builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
          builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 1),
          builder.MakeLong(kTestCustomTagBase + 100, 0x12345678U),
          builder.MakeAscii(kTestCustomTagBase + 101, "anon-str"),
          builder.MakeDouble(kTestCustomTagBase + 102, 3.5),
      };
      if (big_tiff) {
        entries.push_back(
            builder.MakeLong8(kTestCustomTagBase + 103, 0x100000002ULL));
      }
      builder.AddIfd(std::move(entries));

      MemoryTiffStream stream(builder.Take());
      UniqueTiffOptions opts = MakeUniqueTiffOptions();
      stream.diagnostics().Install(opts.get());
      TIFFOpenOptionsSetWarnAboutUnknownTags(opts.get(), 1);
      UniqueTiff tif = stream.Open("r", opts.get());
      ASSERT_NE(tif, nullptr);

      const TIFFField* f_long =
          TIFFFindField(tif.get(), kTestCustomTagBase + 100, TIFF_ANY);
      ASSERT_NE(f_long, nullptr);
      EXPECT_EQ(TIFFFieldIsAnonymous(f_long), 1);
      uint32_t count32 = 0;
      const uint32_t* val_u32 = nullptr;
      ASSERT_EQ(
          TIFFGetField(tif.get(), kTestCustomTagBase + 100, &count32, &val_u32),
          1);
      ASSERT_EQ(count32, 1U);
      EXPECT_EQ(val_u32[0], 0x12345678U);

      const char* val_str = nullptr;
      ASSERT_EQ(
          TIFFGetField(tif.get(), kTestCustomTagBase + 101, &count32, &val_str),
          1);
      EXPECT_STREQ(val_str, "anon-str");

      const double* val_dbl = nullptr;
      ASSERT_EQ(
          TIFFGetField(tif.get(), kTestCustomTagBase + 102, &count32, &val_dbl),
          1);
      ASSERT_EQ(count32, 1U);
      EXPECT_DOUBLE_EQ(val_dbl[0], 3.5);

      if (big_tiff) {
        const uint64_t* val_u64 = nullptr;
        ASSERT_EQ(TIFFGetField(tif.get(), kTestCustomTagBase + 103, &count32,
                               &val_u64),
                  1);
        ASSERT_EQ(count32, 1U);
        EXPECT_EQ(val_u64[0], 0x100000002ULL);
      }

      // Calling TIFFCreateDirectory triggers _TIFFSetupFields to free anons.
      EXPECT_EQ(TIFFCreateDirectory(tif.get()), 0);
      EXPECT_EQ(TIFFFindField(tif.get(), kTestCustomTagBase + 100, TIFF_ANY),
                nullptr);
      EXPECT_EQ(stream.diagnostics().error_count(), 0);
      EXPECT_GE(stream.diagnostics().warning_count(), 3U);
    }
  }
}

TEST(TifDirInfoTest, AnonFieldAndSetupFieldsAllocationFailures) {
  MemoryTiffStream stream;
  UniqueTiff tif = OpenMemoryWithCumulatedBudget(stream, "w");
  ASSERT_NE(tif, nullptr);

  // 1. Fail first allocation (sizeof(TIFFField)) in _TIFFCreateAnonField.
  {
    ScopedSingleMemAllocLimit limit(tif.get(), 1);
    EXPECT_EQ(
        _TIFFCreateAnonField(tif.get(), kTestCustomTagBase + 200, TIFF_LONG),
        nullptr);
    EXPECT_EQ(_TIFFFindOrRegisterField(tif.get(), kTestCustomTagBase + 200,
                                       TIFF_LONG),
              nullptr);
  }

  // 2. Succeed first allocation (sizeof(TIFFField)), fail 32-byte name alloc.
  {
    const tmsize_t cur_before = tif->tif_cur_cumulated_mem_alloc;
    ScopedCumulatedMemAllocRemaining budget(
        tif.get(), static_cast<tmsize_t>(sizeof(TIFFField)));
    EXPECT_EQ(
        _TIFFCreateAnonField(tif.get(), kTestCustomTagBase + 201, TIFF_LONG),
        nullptr);
    EXPECT_EQ(tif->tif_cur_cumulated_mem_alloc, cur_before);
  }

  // 3. Fail _TIFFMergeFields inside _TIFFSetupFields (tif_fields becomes NULL).
  {
    ScopedSingleMemAllocLimit limit(tif.get(), 1);
    _TIFFSetupFields(tif.get(), _TIFFGetFields());
  }
  EXPECT_EQ(tif->tif_fields, nullptr);
  EXPECT_EQ(tif->tif_nfields, 0U);
  EXPECT_GE(stream.diagnostics().error_count(), 1U);

  // Restore fields so TIFFClose finishes cleanly.
  _TIFFSetupFields(tif.get(), _TIFFGetFields());
  EXPECT_NE(tif->tif_fields, nullptr);
}

TEST(TifDirInfoTest, MergeFieldsSkipsDuplicatesAndHandlesNullFieldName) {
  MemoryTiffStream stream;
  UniqueTiff tif = OpenMemoryWithCumulatedBudget(stream, "w");
  ASSERT_NE(tif, nullptr);

  const size_t orig_count = tif->tif_nfields;
  const TIFFField* orig_width =
      TIFFFindField(tif.get(), TIFFTAG_IMAGEWIDTH, TIFF_ANY);
  ASSERT_NE(orig_width, nullptr);

  // Duplicate tag definition is skipped by _TIFFMergeFields.
  TIFFField dup_width = *orig_width;
  dup_width.field_name = "DuplicateWidthIgnored";
  EXPECT_EQ(_TIFFMergeFields(tif.get(), &dup_width, 1), 1);
  EXPECT_EQ(tif->tif_nfields, orig_count);
  EXPECT_EQ(TIFFFindField(tif.get(), TIFFTAG_IMAGEWIDTH, TIFF_ANY), orig_width);

  // Merge a custom field whose field_name is nullptr, then exercise
  // _TIFFSetupFields cleanup checking fld->field_name != NULL.
  TIFFField null_name_field{};
  null_name_field.field_tag = kTestCustomTagBase + 250;
  null_name_field.field_readcount = 1;
  null_name_field.field_writecount = 1;
  null_name_field.field_type = TIFF_LONG;
  null_name_field.set_get_field_type = TIFF_SETGET_UINT32;
  null_name_field.field_bit = FIELD_CUSTOM;
  null_name_field.field_name = nullptr;
  EXPECT_EQ(_TIFFMergeFields(tif.get(), &null_name_field, 1), 1);
  EXPECT_EQ(tif->tif_nfields, orig_count + 1);
  EXPECT_EQ(TIFFFindField(tif.get(), kTestCustomTagBase + 250, TIFF_ANY),
            &null_name_field);

  _TIFFSetupFields(tif.get(), _TIFFGetFields());
  EXPECT_EQ(tif->tif_nfields, orig_count);
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
}

TEST(TifDirInfoTest, MergeFieldInfoAllSetGetTypesAndRoundTrip) {
  MemoryTiffStream stream;
  // Keep string storage alive longer than writer and stable across push_back.
  std::deque<std::string> names;
  {
    UniqueTiff writer = OpenMemoryWithCumulatedBudget(stream, "w");
    ASSERT_NE(writer, nullptr);

    struct SetGetExpectation {
      TIFFDataType dt;
      int expected_scalar_size;
      int expected_array_size;
      int expected_c16_count_size;
      int expected_c32_count_size;
    };
    constexpr auto kDataTypes = std::to_array<SetGetExpectation>({
        {.dt = TIFF_NOTYPE,
         .expected_scalar_size = 1,
         .expected_array_size = 1,
         .expected_c16_count_size = 0,
         .expected_c32_count_size = 0},
        {.dt = TIFF_BYTE,
         .expected_scalar_size = 1,
         .expected_array_size = 1,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_UNDEFINED,
         .expected_scalar_size = 1,
         .expected_array_size = 1,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_ASCII,
         .expected_scalar_size = 1,
         .expected_array_size = 1,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_SHORT,
         .expected_scalar_size = 2,
         .expected_array_size = 2,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_LONG,
         .expected_scalar_size = 4,
         .expected_array_size = 4,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_RATIONAL,
         .expected_scalar_size = 4,
         .expected_array_size = 4,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_SRATIONAL,
         .expected_scalar_size = 4,
         .expected_array_size = 4,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_FLOAT,
         .expected_scalar_size = 4,
         .expected_array_size = 4,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_SBYTE,
         .expected_scalar_size = 1,
         .expected_array_size = 1,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_SSHORT,
         .expected_scalar_size = 2,
         .expected_array_size = 2,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_SLONG,
         .expected_scalar_size = 4,
         .expected_array_size = 4,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_DOUBLE,
         .expected_scalar_size = 8,
         .expected_array_size = 8,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_IFD,
         .expected_scalar_size = 8,
         .expected_array_size = 8,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_IFD8,
         .expected_scalar_size = 8,
         .expected_array_size = 8,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_LONG8,
         .expected_scalar_size = 8,
         .expected_array_size = 8,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = TIFF_SLONG8,
         .expected_scalar_size = 8,
         .expected_array_size = 8,
         .expected_c16_count_size = 2,
         .expected_c32_count_size = 4},
        {.dt = static_cast<TIFFDataType>(19),
         .expected_scalar_size = 1,
         .expected_array_size = 1,
         .expected_c16_count_size = 0,
         .expected_c32_count_size = 0},
    });

    std::vector<TIFFFieldInfo> infos;
    struct ExpectedFieldCheck {
      uint32_t tag;
      int expected_size;
      int expected_count_size;
      bool expect_empty_name;
    };
    std::vector<ExpectedFieldCheck> checks;

    uint32_t next_tag = kTestCustomTagBase + 300;
    auto push_info = [&](int16_t rcount, int16_t wcount, TIFFDataType dt,
                         uint16_t bit, uint8_t passcount,
                         const char* explicit_name, int exp_size,
                         int exp_count_size) {
      const uint32_t tag = next_tag++;
      const char* name_ptr = explicit_name;
      if (explicit_name != nullptr) {
        names.push_back(absl::StrFormat("Custom_%u", tag));
        name_ptr = names.back().c_str();
      }
      infos.push_back({
          .field_tag = tag,
          .field_readcount = rcount,
          .field_writecount = wcount,
          .field_type = dt,
          .field_bit = bit,
          .field_oktochange = 1,
          .field_passcount = passcount,
          .field_name = name_ptr,
      });
      checks.push_back({
          .tag = tag,
          .expected_size = exp_size,
          .expected_count_size = exp_count_size,
          .expect_empty_name = (explicit_name == nullptr),
      });
    };

    // 1. TIFF_ASCII + TIFF_VARIABLE + passcount=0 -> TIFF_SETGET_ASCII.
    push_info(TIFF_VARIABLE, TIFF_VARIABLE, TIFF_ASCII, FIELD_CUSTOM, 0, "a", 1,
              0);

    // 2. count=1, passcount=0 across all TIFFDataType values.
    for (const auto& entry : kDataTypes) {
      push_info(1, 1, entry.dt, FIELD_CUSTOM, 0, "b",
                entry.expected_scalar_size, 0);
    }

    // 3. count=2, passcount=0 across all TIFFDataType values (C0).
    for (const auto& entry : kDataTypes) {
      push_info(2, 2, entry.dt, FIELD_CUSTOM, 0, "c", entry.expected_array_size,
                0);
    }

    // 4. count=TIFF_VARIABLE, passcount=1 across all TIFFDataType values (C16).
    for (const auto& entry : kDataTypes) {
      push_info(TIFF_VARIABLE, TIFF_VARIABLE, entry.dt, FIELD_CUSTOM, 1, "d",
                entry.expected_array_size, entry.expected_c16_count_size);
    }

    // 5. count=TIFF_VARIABLE2, passcount=1 across all TIFFDataType values
    // (C32).
    for (const auto& entry : kDataTypes) {
      push_info(TIFF_VARIABLE2, TIFF_VARIABLE2, entry.dt, FIELD_CUSTOM, 1, "e",
                entry.expected_array_size, entry.expected_c32_count_size);
    }

    // 6. Fallback -> TIFF_SETGET_UNDEFINED, plus field_name == NULL and
    // zero-count FIELD_IGNORE.
    push_info(1, 1, TIFF_LONG, FIELD_CUSTOM, 1, nullptr, 1, 0);
    push_info(TIFF_SPP, TIFF_SPP, TIFF_SHORT, FIELD_CUSTOM, 0, "spp", 1, 0);
    push_info(0, 0, TIFF_SHORT, FIELD_IGNORE, 0, "ign", 1, 0);

    ASSERT_EQ(TIFFMergeFieldInfo(writer.get(), infos.data(),
                                 static_cast<uint32_t>(infos.size())),
              0);

    for (const auto& chk : checks) {
      SCOPED_TRACE(chk.tag);
      const TIFFField* fip = TIFFFindField(writer.get(), chk.tag, TIFF_ANY);
      ASSERT_NE(fip, nullptr);
      EXPECT_EQ(TIFFFieldIsAnonymous(fip), 0);
      EXPECT_EQ(TIFFFieldSetGetSize(fip), chk.expected_size);
      EXPECT_EQ(TIFFFieldSetGetCountSize(fip), chk.expected_count_size);
      if (chk.expect_empty_name) {
        EXPECT_STREQ(TIFFFieldName(fip), "");
      }
    }

    // Second TIFFMergeFieldInfo call exercises tif_nfieldscompat > 0 realloc.
    const std::array<TIFFFieldInfo, 3> extra_infos = {{
        {.field_tag = kTestCustomTagBase + 500,
         .field_readcount = 1,
         .field_writecount = 1,
         .field_type = TIFF_DOUBLE,
         .field_bit = FIELD_CUSTOM,
         .field_oktochange = 1,
         .field_passcount = 0,
         .field_name = "ExtraDouble"},
        {.field_tag = kTestCustomTagBase + 501,
         .field_readcount = TIFF_VARIABLE,
         .field_writecount = TIFF_VARIABLE,
         .field_type = TIFF_SLONG,
         .field_bit = FIELD_CUSTOM,
         .field_oktochange = 1,
         .field_passcount = 1,
         .field_name = "ExtraSlongArray"},
        {.field_tag = kTestCustomTagBase + 502,
         .field_readcount = TIFF_VARIABLE,
         .field_writecount = TIFF_VARIABLE,
         .field_type = TIFF_ASCII,
         .field_bit = FIELD_CUSTOM,
         .field_oktochange = 1,
         .field_passcount = 0,
         .field_name = "ExtraAscii"},
    }};
    ASSERT_EQ(TIFFMergeFieldInfo(writer.get(), extra_infos.data(),
                                 static_cast<uint32_t>(extra_infos.size())),
              0);

    WriteSingleGrayPixelImage(writer.get(), 17);
    ASSERT_EQ(
        TIFFSetField(writer.get(), kTestCustomTagBase + 500, double{2.25}), 1);
    constexpr int32_t kSlongs[2] = {-42, 1337};
    ASSERT_EQ(TIFFSetField(writer.get(), kTestCustomTagBase + 501, uint16_t{2},
                           kSlongs),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), kTestCustomTagBase + 502, "merged-text"), 1);

    // Read back immediately on writer via the merged TIFF_SETGET_* types.
    double writer_double = 0.0;
    ASSERT_EQ(
        TIFFGetField(writer.get(), kTestCustomTagBase + 500, &writer_double),
        1);
    EXPECT_DOUBLE_EQ(writer_double, 2.25);

    uint16_t writer_slong_count = 0;
    const int32_t* writer_slong_ptr = nullptr;
    ASSERT_EQ(TIFFGetField(writer.get(), kTestCustomTagBase + 501,
                           &writer_slong_count, &writer_slong_ptr),
              1);
    ASSERT_EQ(writer_slong_count, 2U);
    EXPECT_EQ(writer_slong_ptr[0], -42);
    EXPECT_EQ(writer_slong_ptr[1], 1337);

    const char* writer_ascii = nullptr;
    ASSERT_EQ(
        TIFFGetField(writer.get(), kTestCustomTagBase + 502, &writer_ascii), 1);
    EXPECT_STREQ(writer_ascii, "merged-text");

    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  // Open header-only so TIFFMergeFieldInfo can inspect field definitions
  // without prior anonymous auto-registration.
  const std::array<TIFFFieldInfo, 3> read_infos = {{
      {.field_tag = kTestCustomTagBase + 500,
       .field_readcount = 1,
       .field_writecount = 1,
       .field_type = TIFF_DOUBLE,
       .field_bit = FIELD_CUSTOM,
       .field_oktochange = 1,
       .field_passcount = 0,
       .field_name = "ExtraDouble"},
      {.field_tag = kTestCustomTagBase + 501,
       .field_readcount = TIFF_VARIABLE,
       .field_writecount = TIFF_VARIABLE,
       .field_type = TIFF_SLONG,
       .field_bit = FIELD_CUSTOM,
       .field_oktochange = 1,
       .field_passcount = 1,
       .field_name = "ExtraSlongArray"},
      {.field_tag = kTestCustomTagBase + 502,
       .field_readcount = TIFF_VARIABLE,
       .field_writecount = TIFF_VARIABLE,
       .field_type = TIFF_ASCII,
       .field_bit = FIELD_CUSTOM,
       .field_oktochange = 1,
       .field_passcount = 0,
       .field_name = "ExtraAscii"},
  }};
  {
    UniqueTiff header_reader = OpenMemoryWithCumulatedBudget(stream, "rh");
    ASSERT_NE(header_reader, nullptr);
    ASSERT_EQ(TIFFMergeFieldInfo(header_reader.get(), read_infos.data(),
                                 static_cast<uint32_t>(read_infos.size())),
              0);
    EXPECT_NE(TIFFFieldWithName(header_reader.get(), "ExtraDouble"), nullptr);
    EXPECT_NE(TIFFFieldWithName(header_reader.get(), "ExtraSlongArray"),
              nullptr);
    EXPECT_NE(TIFFFieldWithName(header_reader.get(), "ExtraAscii"), nullptr);
  }

  // Normal read without tag extender auto-registers written custom tags as
  // anonymous (TIFF_SETGET_C32_* -> uint32_t count + pointer).
  UniqueTiff reader = OpenMemoryWithCumulatedBudget(stream, "r");
  ASSERT_NE(reader, nullptr);

  uint32_t count32 = 0;
  const double* dbl_ptr = nullptr;
  ASSERT_EQ(
      TIFFGetField(reader.get(), kTestCustomTagBase + 500, &count32, &dbl_ptr),
      1);
  ASSERT_EQ(count32, 1U);
  EXPECT_DOUBLE_EQ(dbl_ptr[0], 2.25);

  const int32_t* slong_ptr = nullptr;
  ASSERT_EQ(TIFFGetField(reader.get(), kTestCustomTagBase + 501, &count32,
                         &slong_ptr),
            1);
  ASSERT_EQ(count32, 2U);
  EXPECT_EQ(slong_ptr[0], -42);
  EXPECT_EQ(slong_ptr[1], 1337);

  const char* extra_ascii = nullptr;
  ASSERT_EQ(TIFFGetField(reader.get(), kTestCustomTagBase + 502, &count32,
                         &extra_ascii),
            1);
  EXPECT_STREQ(extra_ascii, "merged-text");
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
}

TEST(TifDirInfoTest, MergeFieldInfoValidationAndAllocationErrors) {
  {
    MemoryTiffStream stream;
    UniqueTiff tif = OpenMemoryWithCumulatedBudget(stream, "w");
    ASSERT_NE(tif, nullptr);

    const TIFFFieldInfo bad_readcount = {
        .field_tag = kTestCustomTagBase + 600,
        .field_readcount = -4,
        .field_writecount = 1,
        .field_type = TIFF_LONG,
        .field_bit = FIELD_CUSTOM,
        .field_oktochange = 1,
        .field_passcount = 0,
        .field_name = "BadReadCount",
    };
    EXPECT_EQ(TIFFMergeFieldInfo(tif.get(), &bad_readcount, 1), -1);

    const TIFFFieldInfo bad_writecount = {
        .field_tag = kTestCustomTagBase + 601,
        .field_readcount = 1,
        .field_writecount = -4,
        .field_type = TIFF_LONG,
        .field_bit = FIELD_CUSTOM,
        .field_oktochange = 1,
        .field_passcount = 0,
        .field_name = "BadWriteCount",
    };
    EXPECT_EQ(TIFFMergeFieldInfo(tif.get(), &bad_writecount, 1), -1);

    const TIFFFieldInfo bad_zero_readcount = {
        .field_tag = kTestCustomTagBase + 602,
        .field_readcount = 0,
        .field_writecount = 1,
        .field_type = TIFF_LONG,
        .field_bit = FIELD_CUSTOM,
        .field_oktochange = 1,
        .field_passcount = 0,
        .field_name = "BadZeroRead",
    };
    EXPECT_EQ(TIFFMergeFieldInfo(tif.get(), &bad_zero_readcount, 1), -1);

    const TIFFFieldInfo bad_zero_writecount = {
        .field_tag = kTestCustomTagBase + 603,
        .field_readcount = 1,
        .field_writecount = 0,
        .field_type = TIFF_LONG,
        .field_bit = FIELD_CUSTOM,
        .field_oktochange = 1,
        .field_passcount = 0,
        .field_name = "BadZeroWrite",
    };
    EXPECT_EQ(TIFFMergeFieldInfo(tif.get(), &bad_zero_writecount, 1), -1);
    EXPECT_EQ(stream.diagnostics().error_count(), 4U);
  }

  // OOM 1: fail allocation of tif_fieldscompat array itself.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = OpenMemoryWithCumulatedBudget(stream, "w");
    ASSERT_NE(tif, nullptr);
    const TIFFFieldInfo valid_info = {
        .field_tag = kTestCustomTagBase + 604,
        .field_readcount = 1,
        .field_writecount = 1,
        .field_type = TIFF_LONG,
        .field_bit = FIELD_CUSTOM,
        .field_oktochange = 1,
        .field_passcount = 0,
        .field_name = "Oom1",
    };
    ScopedSingleMemAllocLimit limit(tif.get(), 1);
    EXPECT_EQ(TIFFMergeFieldInfo(tif.get(), &valid_info, 1), -1);
  }

  // OOM 2: succeed tif_fieldscompat (24 bytes), fail 10 * sizeof(TIFFField).
  {
    MemoryTiffStream stream;
    UniqueTiff tif = OpenMemoryWithCumulatedBudget(stream, "w");
    ASSERT_NE(tif, nullptr);
    std::array<TIFFFieldInfo, 10> info10{};
    for (size_t i = 0; i < info10.size(); ++i) {
      info10[i] = {
          .field_tag = static_cast<uint32_t>(kTestCustomTagBase + 610 + i),
          .field_readcount = 1,
          .field_writecount = 1,
          .field_type = TIFF_LONG,
          .field_bit = FIELD_CUSTOM,
          .field_oktochange = 1,
          .field_passcount = 0,
          .field_name = "Oom2",
      };
    }
    ScopedSingleMemAllocLimit limit(
        tif.get(), static_cast<tmsize_t>(sizeof(TIFFFieldArray)));
    EXPECT_EQ(TIFFMergeFieldInfo(tif.get(), info10.data(),
                                 static_cast<uint32_t>(info10.size())),
              -1);
  }

  // OOM 3: fail _TIFFMergeFields inside TIFFMergeFieldInfo when tif_fields is
  // NULL (so no existing tif_fields pointer is orphaned).
  {
    MemoryTiffStream stream;
    UniqueTiff tif = OpenMemoryWithCumulatedBudget(stream, "w");
    ASSERT_NE(tif, nullptr);
    {
      ScopedSingleMemAllocLimit limit(tif.get(), 1);
      _TIFFSetupFields(tif.get(), _TIFFGetFields());
    }
    ASSERT_EQ(tif->tif_fields, nullptr);

    std::array<TIFFFieldInfo, 10> info10{};
    for (size_t i = 0; i < info10.size(); ++i) {
      info10[i] = {
          .field_tag = static_cast<uint32_t>(kTestCustomTagBase + 630 + i),
          .field_readcount = 1,
          .field_writecount = 1,
          .field_type = TIFF_LONG,
          .field_bit = FIELD_CUSTOM,
          .field_oktochange = 1,
          .field_passcount = 0,
          .field_name = "Oom3",
      };
    }
    {
      ScopedCumulatedMemAllocRemaining budget(
          tif.get(), static_cast<tmsize_t>(sizeof(TIFFFieldArray) +
                                           info10.size() * sizeof(TIFFField)));
      EXPECT_EQ(TIFFMergeFieldInfo(tif.get(), info10.data(),
                                   static_cast<uint32_t>(info10.size())),
                -1);
    }
    _TIFFSetupFields(tif.get(), _TIFFGetFields());
  }
}

TEST(TifDirInfoTest, PrintFieldInfoWritesFormattedTable) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ASSERT_NE(
      _TIFFFindOrRegisterField(tif.get(), kTestCustomTagBase + 700, TIFF_LONG),
      nullptr);

  TempFile out_file("tif_dirinfo_print.txt");
  std::unique_ptr<std::FILE, decltype(&std::fclose)> fp(
      std::fopen(out_file.c_str(), "w+"), &std::fclose);
  ASSERT_NE(fp, nullptr);

  _TIFFPrintFieldInfo(tif.get(), fp.get());
  ASSERT_EQ(std::fflush(fp.get()), 0);
  std::rewind(fp.get());

  std::string output;
  std::array<char, 512> buf{};
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), fp.get()) !=
         nullptr) {
    output.append(buf.data());
  }

  EXPECT_THAT(output, HasSubstr("memory.tiff:"));
  EXPECT_THAT(output, HasSubstr("ImageWidth"));
  EXPECT_THAT(output, HasSubstr("SubIFD"));
  EXPECT_THAT(output, HasSubstr("TRUE"));
  EXPECT_THAT(output, HasSubstr("FALSE"));
  EXPECT_THAT(output,
              HasSubstr(absl::StrFormat("Tag %u", kTestCustomTagBase + 700)));
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
}

TEST(TifDirInfoTest, CheckFieldIsValidForCodecAllSchemes) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  // Non-codec tag is always valid regardless of compression.
  tif->tif_dir.td_compression = COMPRESSION_JBIG;
  EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_IMAGEWIDTH), 1);

  // Unconfigured codec rejects codec-specific tags.
  EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_PREDICTOR), 0);

  // Configured codecs without mock registration.
  const std::array<uint16_t, 6> predictor_codecs = {
      COMPRESSION_LZW,      COMPRESSION_DEFLATE, COMPRESSION_ADOBE_DEFLATE,
      COMPRESSION_PIXARLOG, COMPRESSION_LZMA,    COMPRESSION_ZSTD,
  };
  for (uint16_t scheme : predictor_codecs) {
    SCOPED_TRACE(scheme);
    ASSERT_EQ(TIFFIsCODECConfigured(scheme), 1);
    tif->tif_dir.td_compression = scheme;
    EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_PREDICTOR), 1);
    EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_JPEGTABLES), 0);
  }

  const std::array<uint16_t, 6> no_tag_codecs = {
      COMPRESSION_NONE, COMPRESSION_PACKBITS, COMPRESSION_THUNDERSCAN,
      COMPRESSION_NEXT, COMPRESSION_SGILOG,   COMPRESSION_SGILOG24,
  };
  for (uint16_t scheme : no_tag_codecs) {
    SCOPED_TRACE(scheme);
    ASSERT_EQ(TIFFIsCODECConfigured(scheme), 1);
    tif->tif_dir.td_compression = scheme;
    EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_PREDICTOR), 0);
  }

  // JPEG
  ASSERT_EQ(TIFFIsCODECConfigured(COMPRESSION_JPEG), 1);
  tif->tif_dir.td_compression = COMPRESSION_JPEG;
  EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_JPEGTABLES), 1);
  EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_PREDICTOR), 0);

  // CCITT family
  const std::array<uint16_t, 4> ccitt_codecs = {
      COMPRESSION_CCITTRLE,
      COMPRESSION_CCITTRLEW,
      COMPRESSION_CCITTFAX3,
      COMPRESSION_CCITTFAX4,
  };
  for (uint16_t scheme : ccitt_codecs) {
    SCOPED_TRACE(scheme);
    ASSERT_EQ(TIFFIsCODECConfigured(scheme), 1);
    tif->tif_dir.td_compression = scheme;
    EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_BADFAXLINES),
              1);
    EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_CLEANFAXDATA),
              1);
    EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(),
                                             TIFFTAG_CONSECUTIVEBADFAXLINES),
              1);
    EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_GROUP3OPTIONS),
              scheme == COMPRESSION_CCITTFAX3 ? 1 : 0);
    EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_GROUP4OPTIONS),
              scheme == COMPRESSION_CCITTFAX4 ? 1 : 0);
    EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_PREDICTOR), 0);
  }

  // LERC
  ASSERT_EQ(TIFFIsCODECConfigured(COMPRESSION_LERC), 1);
  tif->tif_dir.td_compression = COMPRESSION_LERC;
  EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_LERC_PARAMETERS),
            1);
  EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_PREDICTOR), 0);

  // Temporarily register OJPEG and JBIG so their switch branches are reachable.
  {
    ScopedRegisteredCodec mock_ojpeg(COMPRESSION_OJPEG, "MockOJPEG",
                                     &DummyCodecInit);
    ASSERT_NE(mock_ojpeg.get(), nullptr);
    ASSERT_EQ(TIFFIsCODECConfigured(COMPRESSION_OJPEG), 1);
    tif->tif_dir.td_compression = COMPRESSION_OJPEG;
    const std::array<uint32_t, 7> ojpeg_tags = {
        TIFFTAG_JPEGIFOFFSET,        TIFFTAG_JPEGIFBYTECOUNT,
        TIFFTAG_JPEGQTABLES,         TIFFTAG_JPEGDCTABLES,
        TIFFTAG_JPEGACTABLES,        TIFFTAG_JPEGPROC,
        TIFFTAG_JPEGRESTARTINTERVAL,
    };
    for (uint32_t tag : ojpeg_tags) {
      EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), tag), 1);
    }
    EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_PREDICTOR), 0);
  }
  {
    ScopedRegisteredCodec mock_jbig(COMPRESSION_JBIG, "MockJBIG",
                                    &DummyCodecInit);
    ASSERT_NE(mock_jbig.get(), nullptr);
    ASSERT_EQ(TIFFIsCODECConfigured(COMPRESSION_JBIG), 1);
    tif->tif_dir.td_compression = COMPRESSION_JBIG;
    EXPECT_EQ(_TIFFCheckFieldIsValidForCodec(tif.get(), TIFFTAG_PREDICTOR), 0);
  }
  tif->tif_dir.td_compression = COMPRESSION_NONE;
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
}

TEST(TifDirInfoTest, CodecFieldsetsMergedOnCompressionSet) {
  struct CodecTagCheck {
    uint16_t scheme;
    uint32_t expected_tag;
    std::string_view expected_name;
  };
  constexpr auto kCodecTagChecks = std::to_array<CodecTagCheck>({
      {.scheme = COMPRESSION_LZW,
       .expected_tag = TIFFTAG_PREDICTOR,
       .expected_name = "Predictor"},
      {.scheme = COMPRESSION_ADOBE_DEFLATE,
       .expected_tag = TIFFTAG_ZIPQUALITY,
       .expected_name = ""},
      {.scheme = COMPRESSION_JPEG,
       .expected_tag = TIFFTAG_JPEGTABLES,
       .expected_name = "JPEGTables"},
      {.scheme = COMPRESSION_CCITTFAX3,
       .expected_tag = TIFFTAG_GROUP3OPTIONS,
       .expected_name = "Group3Options"},
      {.scheme = COMPRESSION_CCITTFAX4,
       .expected_tag = TIFFTAG_GROUP4OPTIONS,
       .expected_name = "Group4Options"},
      {.scheme = COMPRESSION_SGILOG,
       .expected_tag = TIFFTAG_SGILOGDATAFMT,
       .expected_name = "SGILogDataFmt"},
      {.scheme = COMPRESSION_LZMA,
       .expected_tag = TIFFTAG_LZMAPRESET,
       .expected_name = "LZMA2 Compression Preset"},
      {.scheme = COMPRESSION_ZSTD,
       .expected_tag = TIFFTAG_ZSTD_LEVEL,
       .expected_name = "ZSTD compression_level"},
      {.scheme = COMPRESSION_WEBP,
       .expected_tag = TIFFTAG_WEBP_LEVEL,
       .expected_name = "WEBP quality"},
      {.scheme = COMPRESSION_LERC,
       .expected_tag = TIFFTAG_LERC_PARAMETERS,
       .expected_name = "LercParameters"},
      {.scheme = COMPRESSION_JXL,
       .expected_tag = TIFFTAG_JXL_LOSSYNESS,
       .expected_name = "Lossyness"},
  });

  for (const auto& chk : kCodecTagChecks) {
    if (!TIFFIsCODECConfigured(chk.scheme)) continue;
    SCOPED_TRACE(chk.scheme);
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, chk.scheme), 1);
    const TIFFField* fip = TIFFFindField(tif.get(), chk.expected_tag, TIFF_ANY);
    ASSERT_NE(fip, nullptr);
    if (!chk.expected_name.empty()) {
      const std::string name_str(chk.expected_name);
      EXPECT_EQ(TIFFFieldWithName(tif.get(), name_str.c_str()), fip);
    } else {
      EXPECT_STREQ(TIFFFieldName(fip), "");
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0);
  }
}

void TestTagExtenderCallback(TIFF* tif) {
  static constexpr std::array<TIFFFieldInfo, 2> kExtFields = {{
      {.field_tag = kTestCustomTagBase + 800,
       .field_readcount = 1,
       .field_writecount = 1,
       .field_type = TIFF_LONG,
       .field_bit = FIELD_CUSTOM,
       .field_oktochange = 1,
       .field_passcount = 0,
       .field_name = "ExtenderLong"},
      {.field_tag = kTestCustomTagBase + 801,
       .field_readcount = TIFF_VARIABLE,
       .field_writecount = TIFF_VARIABLE,
       .field_type = TIFF_ASCII,
       .field_bit = FIELD_CUSTOM,
       .field_oktochange = 1,
       .field_passcount = 0,
       .field_name = "ExtenderAscii"},
  }};
  TIFFMergeFieldInfo(tif, kExtFields.data(),
                     static_cast<uint32_t>(kExtFields.size()));
}

TEST(TifDirInfoTest, TagExtenderRoundTrip) {
  ScopedTagExtender extender(&TestTagExtenderCallback);
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    const TIFFField* ext_long = TIFFFieldWithName(writer.get(), "ExtenderLong");
    ASSERT_NE(ext_long, nullptr);
    EXPECT_EQ(TIFFFieldIsAnonymous(ext_long), 0);

    WriteSingleGrayPixelImage(writer.get(), 77);
    ASSERT_EQ(
        TIFFSetField(writer.get(), kTestCustomTagBase + 800, uint32_t{987654}),
        1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), kTestCustomTagBase + 801, "extender-ok"), 1);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  const TIFFField* ext_ascii = TIFFFieldWithName(reader.get(), "ExtenderAscii");
  ASSERT_NE(ext_ascii, nullptr);
  EXPECT_EQ(TIFFFieldIsAnonymous(ext_ascii), 0);

  uint32_t long_val = 0;
  const char* ascii_val = nullptr;
  ASSERT_EQ(TIFFGetField(reader.get(), kTestCustomTagBase + 800, &long_val), 1);
  ASSERT_EQ(TIFFGetField(reader.get(), kTestCustomTagBase + 801, &ascii_val),
            1);
  EXPECT_EQ(long_val, 987654U);
  EXPECT_STREQ(ascii_val, "extender-ok");
  EXPECT_EQ(stream.diagnostics().error_count(), 0);
  EXPECT_EQ(stream.diagnostics().warning_count(), 0);
}

}  // namespace
}  // namespace libtiff
