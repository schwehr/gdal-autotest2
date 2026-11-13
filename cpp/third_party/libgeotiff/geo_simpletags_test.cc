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
// Tests in-memory SimpleTags storage and TIFFMethod dispatch
// (geo_simpletags.c).
//
// See also:
//   https://github.com/OSGeo/libgeotiff/blob/master/libgeotiff/geo_simpletags.c

#include "third_party/libgeotiff/geo_simpletags.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/libgeotiff/geo_tiffp.h"
#include "third_party/libgeotiff/geokeys.h"
#include "third_party/libgeotiff/geotiff.h"
#include "third_party/libgeotiff/geovalues.h"
#include "third_party/libgeotiff/google/util/scoped_geotiff.h"
#include "third_party/libgeotiff/libxtiff/xtiffio.h"  // IWYU pragma: keep

namespace libgeotiff {
namespace {

using ::testing::ElementsAre;

TEST(GeoSimpletagsTest, CreateAndDestroyEmpty) {
  ScopedSimpleTags st(ST_Create());
  ASSERT_NE(nullptr, st);
  EXPECT_EQ(0, st->key_count);
  EXPECT_EQ(nullptr, st->key_list);
}

TEST(GeoSimpletagsTest, SetAndGetTagTypesWithReplacement) {
  ScopedSimpleTags st(ST_Create());
  ASSERT_NE(nullptr, st);

  // 1. Store ASCII with count=0 so ST_SetKey computes strlen + 1.
  char ascii_initial[] = "WGS 84|";
  ASSERT_EQ(1, ST_SetKey(st.get(), TIFFTAG_GEOASCIIPARAMS, 0, STT_ASCII,
                         ascii_initial));

  // 2. Store SHORT and DOUBLE tag arrays.
  std::array<uint16_t, 4> short_vals = {1, 1, 0, 0};
  ASSERT_EQ(1, SetKeyDirectory(st.get(), short_vals));

  std::array<double, 3> pixel_scale = {10.0, 10.0, 0.0};
  ASSERT_EQ(1, SetSimpleTag(st.get(), TIFFTAG_GEOPIXELSCALE, STT_DOUBLE,
                            pixel_scale));

  // 3. Verify stored ASCII tag and optional null out-parameters in ST_GetKey.
  int count = -1;
  int st_type = -1;
  void* data_ptr = nullptr;
  ASSERT_EQ(1, ST_GetKey(st.get(), TIFFTAG_GEOASCIIPARAMS, &count, &st_type,
                         &data_ptr));
  EXPECT_EQ(static_cast<int>(std::strlen(ascii_initial)) + 1, count);
  EXPECT_EQ(STT_ASCII, st_type);
  ASSERT_NE(nullptr, data_ptr);
  EXPECT_STREQ(ascii_initial, static_cast<const char*>(data_ptr));
  EXPECT_EQ(1, ST_GetKey(st.get(), TIFFTAG_GEOASCIIPARAMS, nullptr, nullptr,
                         nullptr));

  // 4. Overwrite the existing ASCII tag with explicit count and confirm
  // replacement updates both count and payload.
  char ascii_updated[] = "NAD83|";
  ASSERT_EQ(1, ST_SetKey(st.get(), TIFFTAG_GEOASCIIPARAMS,
                         static_cast<int>(std::strlen(ascii_updated)) + 1,
                         STT_ASCII, ascii_updated));
  ASSERT_EQ(1, ST_GetKey(st.get(), TIFFTAG_GEOASCIIPARAMS, &count, &st_type,
                         &data_ptr));
  EXPECT_EQ(static_cast<int>(std::strlen(ascii_updated)) + 1, count);
  EXPECT_EQ(STT_ASCII, st_type);
  EXPECT_STREQ(ascii_updated, static_cast<const char*>(data_ptr));

  // 5. Missing tag lookup returns 0.
  EXPECT_EQ(0, ST_GetKey(st.get(), TIFFTAG_GEOTRANSMATRIX, &count, &st_type,
                         &data_ptr));
}

TEST(GeoSimpletagsTest, StTagTypeMapping) {
  struct TagTypeCase {
    int tag;
    int expected_st_type;
  };
  constexpr TagTypeCase kCases[] = {
      {.tag = TIFFTAG_GEOASCIIPARAMS, .expected_st_type = STT_ASCII},
      {.tag = TIFFTAG_GEOPIXELSCALE, .expected_st_type = STT_DOUBLE},
      {.tag = TIFFTAG_GEOTRANSMATRIX, .expected_st_type = STT_DOUBLE},
      {.tag = TIFFTAG_GEOTIEPOINTS, .expected_st_type = STT_DOUBLE},
      {.tag = TIFFTAG_GEODOUBLEPARAMS, .expected_st_type = STT_DOUBLE},
      {.tag = TIFFTAG_GEOKEYDIRECTORY, .expected_st_type = STT_SHORT},
      {.tag = 0, .expected_st_type = -1},
      {.tag = TIFFTAG_IMAGEWIDTH, .expected_st_type = -1},
  };
  for (const auto& tc : kCases) {
    EXPECT_EQ(tc.expected_st_type, ST_TagType(tc.tag)) << "tag=" << tc.tag;
  }
}

TEST(GeoSimpletagsTest, SimpleTagsMethodsDispatchAndTypeMismatch) {
  // nullptr is a documented no-op.
  GTIFSetSimpleTagsMethods(nullptr);

  TIFFMethod methods{};
  GTIFSetSimpleTagsMethods(&methods);
  ASSERT_NE(nullptr, methods.get);
  ASSERT_NE(nullptr, methods.set);
  ASSERT_NE(nullptr, methods.type);

  ScopedSimpleTags st(ST_Create());
  ASSERT_NE(nullptr, st);
  auto* tiff_handle = reinterpret_cast<tiff_t*>(st.get());

  // 1. Check _GTIFTagType mapping across all supported GeoTIFF tags and
  // unknown.
  struct MethodTypeCase {
    pinfo_t tag;
    tagtype_t expected;
  };
  constexpr MethodTypeCase kMethodTypes[] = {
      {.tag = GTIFF_ASCIIPARAMS, .expected = TYPE_ASCII},
      {.tag = GTIFF_PIXELSCALE, .expected = TYPE_DOUBLE},
      {.tag = GTIFF_TRANSMATRIX, .expected = TYPE_DOUBLE},
      {.tag = GTIFF_TIEPOINTS, .expected = TYPE_DOUBLE},
      {.tag = GTIFF_DOUBLEPARAMS, .expected = TYPE_DOUBLE},
      {.tag = GTIFF_GEOKEYDIRECTORY, .expected = TYPE_SHORT},
      {.tag = TIFFTAG_IMAGEWIDTH, .expected = TYPE_UNKNOWN},
  };
  for (const auto& tc : kMethodTypes) {
    EXPECT_EQ(tc.expected, methods.type(tiff_handle, tc.tag))
        << "tag=" << tc.tag;
  }

  // 2. Missing tag returns 0 via methods.get.
  int count = 0;
  void* raw_val = nullptr;
  EXPECT_EQ(0, methods.get(tiff_handle, GTIFF_DOUBLEPARAMS, &count, &raw_val));
  EXPECT_EQ(nullptr, raw_val);

  // 3. Round-trip via methods.set / methods.get (caller owns returned buffer
  // allocated via _GTIFcalloc, freed via _GTIFFree).
  std::array<double, 2> doubles = {42.5, -17.25};
  ASSERT_EQ(1, methods.set(tiff_handle, GTIFF_DOUBLEPARAMS,
                           static_cast<int>(doubles.size()), doubles.data()));
  ASSERT_EQ(1, methods.get(tiff_handle, GTIFF_DOUBLEPARAMS, &count, &raw_val));
  ScopedRawGtifMem val_guard(raw_val);
  ASSERT_EQ(2, count);
  const auto* retrieved = static_cast<const double*>(val_guard.get());
  const std::array<double, 2> retrieved_values = {retrieved[0], retrieved[1]};
  EXPECT_THAT(retrieved_values, ElementsAre(42.5, -17.25));

  // 4. Tag stored with mismatched ST type is rejected by _GTIFGetField.
  std::array<uint16_t, 3> wrong_type = {1, 2, 3};
  ASSERT_EQ(
      1, SetSimpleTag(st.get(), TIFFTAG_GEOPIXELSCALE, STT_SHORT, wrong_type));
  void* mismatched_val = nullptr;
  EXPECT_EQ(
      0, methods.get(tiff_handle, GTIFF_PIXELSCALE, &count, &mismatched_val));
  EXPECT_EQ(nullptr, mismatched_val);
}

TEST(GeoSimpletagsTest, GtifNewSimpleTagsWriteAndReopenRoundTrip) {
  ScopedSimpleTags st(ST_Create());
  ASSERT_NE(nullptr, st);
  {
    ScopedGtif writer(GTIFNewSimpleTags(st.get()));
    ASSERT_NE(nullptr, writer);
    ASSERT_EQ(1, GTIFKeySet(writer.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                            ModelTypeGeographic));
    ASSERT_EQ(1, GTIFKeySet(writer.get(), GTCitationGeoKey, TYPE_ASCII, 0,
                            "SimpleTags Cit"));
    ASSERT_EQ(1, GTIFKeySet(writer.get(), GeogSemiMajorAxisGeoKey, TYPE_DOUBLE,
                            1, 6378137.0));
    ASSERT_EQ(1, GTIFWriteKeys(writer.get()));
  }

  ScopedGtif reader(GTIFNewSimpleTags(st.get()));
  ASSERT_NE(nullptr, reader);
  uint16_t model_type = 0;
  EXPECT_EQ(
      1, GTIFKeyGetSHORT(reader.get(), GTModelTypeGeoKey, &model_type, 0, 1));
  EXPECT_EQ(ModelTypeGeographic, model_type);

  std::array<char, 64> citation{};
  EXPECT_EQ(15, GTIFKeyGetASCII(reader.get(), GTCitationGeoKey, citation.data(),
                                static_cast<int>(citation.size())));
  EXPECT_STREQ("SimpleTags Cit", citation.data());

  double semi_major = 0.0;
  EXPECT_EQ(1, GTIFKeyGetDOUBLE(reader.get(), GeogSemiMajorAxisGeoKey,
                                &semi_major, 0, 1));
  EXPECT_DOUBLE_EQ(6378137.0, semi_major);
}

// TODO(schwehr): Simulate allocator failure (`!ret_value`) in ST_Create
// (geo_simpletags.c:73) with fault injection.

}  // namespace
}  // namespace libgeotiff
