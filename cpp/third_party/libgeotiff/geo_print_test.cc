// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests GeoTIFF key/tag dump and import routines (geo_print.c).
//
// Build & safety notes:
//   - GTIFImport multi-valued SHORT parsing in upstream ReadKey is avoided
//     because upstream reads an uninitialized int and passes `sptr` past end.
//   - fmemopen inputs avoid blank lines and include End_Of_Keys. so
//     DefaultRead's fscanf does not leave stack buffers uninitialized under
//     MSan.
//
// See also:
//   https://github.com/OSGeo/libgeotiff/blob/master/libgeotiff/geo_print.c

#include <stdio.h>  // For POSIX fmemopen.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/libgeotiff/geo_simpletags.h"
// clang-format off
#include "third_party/libgeotiff/geotiff.h"
#include "third_party/libgeotiff/geo_tiffp.h"  // IWYU pragma: keep
#include "third_party/libgeotiff/geo_keyp.h"
// clang-format on
#include "third_party/libgeotiff/geokeys.h"
#include "third_party/libgeotiff/geovalues.h"
#include "third_party/libgeotiff/google/util/scoped_geotiff.h"
#include "third_party/libgeotiff/libxtiff/xtiffio.h"  // IWYU pragma: keep

namespace libgeotiff {
namespace {

using ::testing::HasSubstr;

struct LineFeed {
  std::vector<std::string> lines;
  size_t index = 0;
};

int CustomReadLine(char* out, void* aux) {
  auto* feed = static_cast<LineFeed*>(aux);
  if (feed == nullptr || feed->index >= feed->lines.size()) {
    out[0] = '\0';
    return 0;
  }
  const std::string& line = feed->lines[feed->index++];
  // GTIFImport and ReadTag in geo_print.c pass a 1024-byte stack buffer
  // (char message[1024]).
  const size_t n = std::min(line.size(), size_t{1023});
  std::memcpy(out, line.data(), n);
  out[n] = '\0';
  return 1;
}

int CustomPrintAppend(char* str, void* aux) {
  auto* out = static_cast<std::string*>(aux);
  out->append(str);
  return 1;
}

TEST(GeoPrintTest, PrintsAllTagsKeysAndCorruptedEdgeCases) {
  // 1. Handle without backing TIFF exercises PrintGeoTags null early return.
  {
    ScopedGtif null_tif(GTIFNew(nullptr));
    ASSERT_NE(nullptr, null_tif);
    std::string out;
    GTIFPrint(null_tif.get(), CustomPrintAppend, &out);
    EXPECT_THAT(out, HasSubstr("Geotiff_Information:"));
    EXPECT_THAT(out, HasSubstr("End_Of_Geotiff."));
  }

  // 2. Full handle with all 3 TIFF geo tags, escaped/long ASCII (>40 chars),
  // multi-line DOUBLE, single & multi-valued SHORT, and TYPE_UNKNOWN.
  ScopedSimpleTags st(ST_Create());
  ASSERT_NE(nullptr, st);

  std::array<double, 6> tiepoints = {0.0, 0.0, 0.0, 500000.0, 4000000.0, 0.0};
  std::array<double, 3> pixel_scale = {10.0, 10.0, 0.0};
  std::array<double, 16> matrix = {
      10.0, 0.0, 0.0, 500000.0, 0.0, -10.0, 0.0, 4000000.0,
      0.0,  0.0, 1.0, 0.0,      0.0, 0.0,   0.0, 1.0,
  };
  ASSERT_EQ(
      1, SetSimpleTag(st.get(), TIFFTAG_GEOTIEPOINTS, STT_DOUBLE, tiepoints));
  ASSERT_EQ(1, SetSimpleTag(st.get(), TIFFTAG_GEOPIXELSCALE, STT_DOUBLE,
                            pixel_scale));
  ASSERT_EQ(1,
            SetSimpleTag(st.get(), TIFFTAG_GEOTRANSMATRIX, STT_DOUBLE, matrix));

  // Inject a 4-value SHORT key at offset 8 inside GeoKeyDirectory.
  std::array<uint16_t, 12> raw_dir = {
      1, 1, 0, 1, 9001, TIFFTAG_GEOKEYDIRECTORY, 4, 8, 11, 22, 33, 44,
  };
  ASSERT_EQ(1, SetKeyDirectory(st.get(), raw_dir));

  ScopedGtif gtif(GTIFNewSimpleTags(st.get()));
  ASSERT_NE(nullptr, gtif);

  ASSERT_EQ(1, GTIFKeySet(gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeProjected));
  ASSERT_EQ(1, GTIFKeySet(gtif.get(), GTCitationGeoKey, TYPE_ASCII, 0,
                          "Line1\nBack\\Slash "
                          "BufferFlushValidationExceedingFortyCharactersEnd"));
  ASSERT_EQ(1, GTIFKeySet(gtif.get(), GeogSemiMajorAxisGeoKey, TYPE_DOUBLE, 1,
                          6378137.0));
  std::array<double, 4> towgs = {1.0, 2.0, 3.0, 4.0};
  ASSERT_EQ(1, GTIFKeySet(gtif.get(), GeogTOWGS84GeoKey, TYPE_DOUBLE, 4,
                          towgs.data()));

  // Add an extra SHORT key whose internal type we temporarily set to UNKNOWN.
  ASSERT_EQ(1, GTIFKeySet(gtif.get(), GTRasterTypeGeoKey, TYPE_SHORT, 1,
                          RasterPixelIsArea));
  const int unknown_idx = gtif->gt_keyindex[GTRasterTypeGeoKey];
  ASSERT_GT(unknown_idx, 0);
  gtif->gt_keys[unknown_idx].gk_type = TYPE_UNKNOWN;
  absl::Cleanup restore_type = [&] {
    gtif->gt_keys[unknown_idx].gk_type = TYPE_SHORT;
  };

  // Print via DefaultPrint (FILE* from open_memstream) and inspect output.
  const std::string printed =
      CaptureFileOutput([&](FILE* fp) { GTIFPrint(gtif.get(), nullptr, fp); });
  EXPECT_THAT(printed, HasSubstr("ModelTiepointTag (2,3):"));
  EXPECT_THAT(printed, HasSubstr("ModelPixelScaleTag (1,3):"));
  EXPECT_THAT(printed, HasSubstr("ModelTransformationTag (4,4):"));
  EXPECT_THAT(printed, HasSubstr("ModelTypeProjected"));
  EXPECT_THAT(printed, HasSubstr("Line1\\nBack\\\\Slash"));
  EXPECT_THAT(printed,
              HasSubstr("BufferFlushValidationExceedingFortyCharactersEnd"));
  EXPECT_THAT(printed, HasSubstr("6378137"));
  EXPECT_THAT(printed, HasSubstr("Unknown Type"));

  // Temporarily null out multi-valued SHORT gk_data to cover corrupted branch.
  const int multi_idx = gtif->gt_keyindex[9001];
  ASSERT_GT(multi_idx, 0);
  char* const saved_sptr = gtif->gt_keys[multi_idx].gk_data;
  gtif->gt_keys[multi_idx].gk_data = nullptr;
  absl::Cleanup restore_sptr = [&] {
    gtif->gt_keys[multi_idx].gk_data = saved_sptr;
  };
  std::string corrupted_out;
  GTIFPrint(gtif.get(), CustomPrintAppend, &corrupted_out);
  EXPECT_THAT(corrupted_out, HasSubstr("****Corrupted data****"));
}

TEST(GeoPrintTest, ImportsCleanMetadataStreamViaDefaultRead) {
  static constexpr char kMetadata[] =
      "Geotiff_Information:\n"
      "   Version: 1\n"
      "   Key_Revision: 1.0\n"
      "   Tagged_Information:\n"
      "      ModelTiepointTag (2,3):\n"
      "         0 0 0\n"
      "         500000 4000000 0\n"
      "      ModelPixelScaleTag (1,3):\n"
      "         10 10 0\n"
      "      End_Of_Tags.\n"
      "   Keyed_Information:\n"
      "      GTModelTypeGeoKey (Short,1): ModelTypeProjected\n"
      "      GTCitationGeoKey (Ascii,9): \"Line\\n\\\\Ok\"\n"
      "      GeogSemiMajorAxisGeoKey (Double,1): 6378137\n"
      "      GeogTOWGS84GeoKey (Double,4): 1 2 3\n"
      "         4\n"
      "      End_Of_Keys.\n"
      "   End_Of_Geotiff.\n";

  ScopedFile mem_fp(
      fmemopen(const_cast<char*>(kMetadata), sizeof(kMetadata) - 1, "r"));
  ASSERT_NE(nullptr, mem_fp);

  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);
  ASSERT_EQ(1, GTIFImport(handle.gtif.get(), nullptr, mem_fp.get()));

  uint16_t model = 0;
  std::array<char, 16> citation{};
  double semi_major = 0.0;
  std::array<double, 4> towgs{};
  EXPECT_EQ(
      1, GTIFKeyGetSHORT(handle.gtif.get(), GTModelTypeGeoKey, &model, 0, 1));
  EXPECT_EQ(ModelTypeProjected, model);
  EXPECT_EQ(
      9, GTIFKeyGetASCII(handle.gtif.get(), GTCitationGeoKey, citation.data(),
                         static_cast<int>(citation.size())));
  EXPECT_STREQ("Line\n\\Ok", citation.data());
  EXPECT_EQ(1, GTIFKeyGetDOUBLE(handle.gtif.get(), GeogSemiMajorAxisGeoKey,
                                &semi_major, 0, 1));
  EXPECT_DOUBLE_EQ(6378137.0, semi_major);
  EXPECT_EQ(4, GTIFKeyGetDOUBLE(handle.gtif.get(), GeogTOWGS84GeoKey,
                                towgs.data(), 0, 4));
  EXPECT_DOUBLE_EQ(4.0, towgs[3]);

  int tie_count = 0;
  int tie_type = 0;
  void* tie_data = nullptr;
  EXPECT_EQ(1, ST_GetKey(handle.st.get(), TIFFTAG_GEOTIEPOINTS, &tie_count,
                         &tie_type, &tie_data));
  EXPECT_EQ(6, tie_count);
}

TEST(GeoPrintTest, RejectsMalformedHeadersTagsAndKeysViaCustomRead) {
  auto import_lines = [](std::vector<std::string> lines) -> int {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    if (handle.gtif == nullptr) {
      return -1;
    }
    LineFeed feed{.lines = std::move(lines), .index = 0};
    return GTIFImport(handle.gtif.get(), CustomReadLine, &feed);
  };

  // Verify CustomReadLine succeeds on valid unindented lines (note
  // DefaultRead's fscanf trailing '\n' consumes leading indentation on disk
  // dumps).
  EXPECT_EQ(1, import_lines({
                   "Geotiff_Information:",
                   "Version: 1",
                   "Key_Revision: 1.0",
                   "Tagged_Information:",
                   "End_Of_Tags.",
                   "Keyed_Information:",
                   "End_Of_Keys.",
               }));

  // Top-level header failures.
  EXPECT_EQ(0, import_lines({"Not_Geotiff:"}));
  EXPECT_EQ(0, import_lines({"Geotiff_Information:", "BadVersion"}));
  EXPECT_EQ(0, import_lines({"Geotiff_Information:", "Version: 1", "BadRev"}));
  EXPECT_EQ(0, import_lines({"Geotiff_Information:", "Version: 1",
                             "Key_Revision: 1.0", "Not_Tags"}));
  EXPECT_EQ(0, import_lines(
                   {"Geotiff_Information:", "Version: 1", "Key_Revision: 1.0",
                    "Tagged_Information:", "End_Of_Tags.", "Not_Keys"}));

  // ReadTag failures: bad header format, unknown tag name, non-numeric row.
  const std::vector<std::string> kHeaderOk = {
      "Geotiff_Information:",
      "Version: 1",
      "Key_Revision: 1.0",
      "Tagged_Information:",
  };
  auto with_tag = [&](std::vector<std::string> tag_lines) {
    std::vector<std::string> lines = kHeaderOk;
    lines.insert(lines.end(), tag_lines.begin(), tag_lines.end());
    return import_lines(std::move(lines));
  };
  EXPECT_EQ(0, with_tag({"MalformedTagHeader"}));
  EXPECT_EQ(0, with_tag({"UnknownTagName (1,3):"}));
  EXPECT_EQ(0, with_tag({"ModelPixelScaleTag (1,3):", "notnum"}));

  // ReadKey failures and Byte default branch.
  const std::vector<std::string> kKeysOk = {
      "Geotiff_Information:", "Version: 1",   "Key_Revision: 1.0",
      "Tagged_Information:",  "End_Of_Tags.", "Keyed_Information:",
  };
  auto with_key = [&](std::vector<std::string> key_lines) {
    std::vector<std::string> lines = kKeysOk;
    lines.insert(lines.end(), key_lines.begin(), key_lines.end());
    return import_lines(std::move(lines));
  };
  EXPECT_EQ(0, with_key({"MalformedKeyLine"}));
  EXPECT_EQ(0, with_key({"GTModelTypeGeoKey (Short,1)"}));  // missing ':'
  EXPECT_EQ(0, with_key({"BogusGeoKey (Short,1): 1"}));
  EXPECT_EQ(0, with_key({"GTModelTypeGeoKey (BogusType,1): 1"}));
  EXPECT_EQ(0, with_key({"GTModelTypeGeoKey (Short,1):   "}));
  EXPECT_EQ(0, with_key({"GTCitationGeoKey (Ascii,4): NoQuotes"}));
  EXPECT_EQ(0, with_key({"GTCitationGeoKey (Ascii,10): \"Short\""}));
  EXPECT_EQ(0, with_key({"GTCitationGeoKey (Ascii,4): \"abcd"}));
  EXPECT_EQ(0, with_key({"GTModelTypeGeoKey (Short,1): InvalidEnum"}));
  EXPECT_EQ(0, with_key({"GTModelTypeGeoKey (Byte,1): 1"}));
  // Bad double triggers StringError without returning; EOF ends key loop.
#if !defined(MEMORY_SANITIZER)
  // Upstream ReadKey (geo_print.c:435) calls StringError(vptr) without
  // returning when "%lg" sscanf fails, passing uninitialized stack `data[0]`
  // to GTIFKeySet at geo_print.c:446.
  EXPECT_EQ(0, with_key({"GeogSemiMajorAxisGeoKey (Double,1): not_float"}));
#endif
}

// TODO(schwehr): Multi-valued SHORT parsing in ReadKey (geo_print.c:462-484)
// is avoided due to upstream uninitialized reads and passing `sptr` past end;
// DefaultRead blank-line branch (geo_print.c:499-500, `num_read == 0`) can be
// covered if blank-line input is added carefully under MSan.

}  // namespace
}  // namespace libgeotiff
