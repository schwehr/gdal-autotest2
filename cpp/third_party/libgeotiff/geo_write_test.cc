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
// Tests GeoKey sorting and serialization to TIFF tags (geo_write.c).
//
// See also:
//   https://github.com/OSGeo/libgeotiff/blob/master/libgeotiff/geo_write.c

#include <array>
#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/libgeotiff/geo_simpletags.h"
// clang-format off
#include "third_party/libgeotiff/geokeys.h"
#include "third_party/libgeotiff/geotiff.h"
#include "third_party/libgeotiff/geo_tiffp.h"  // IWYU pragma: keep
#include "third_party/libgeotiff/geo_keyp.h"
// clang-format on
#include "third_party/libgeotiff/geovalues.h"
#include "third_party/libgeotiff/google/util/scoped_geotiff.h"
#include "third_party/libgeotiff/libxtiff/xtiffio.h"  // IWYU pragma: keep

namespace libgeotiff {
namespace {

TEST(GeoWriteTest, UnmodifiedAndNullTifReturnExpectedStatus) {
  // 1. Clean newly created handle has FLAG_FILE_MODIFIED unset -> returns 1.
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);
  EXPECT_EQ(1, GTIFWriteKeys(handle.gtif.get()));

  // 2. Modified handle backed by gt_tif == nullptr fails with 0.
  ScopedGtif null_tif_gtif(GTIFNew(nullptr));
  ASSERT_NE(nullptr, null_tif_gtif);
  ASSERT_EQ(1, GTIFKeySet(null_tif_gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeProjected));
  EXPECT_EQ(0, GTIFWriteKeys(null_tif_gtif.get()));
}

TEST(GeoWriteTest, SortsUnsortedKeysAndSerializesAllTagTypes) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  // 1. Insert keys in descending numerical order to exercise SortKeys swaps.
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), ProjectedCSTypeGeoKey, TYPE_SHORT,
                          1, PCS_WGS84_UTM_zone_10N));
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogSemiMajorAxisGeoKey,
                          TYPE_DOUBLE, 1, 6378137.0));
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTCitationGeoKey, TYPE_ASCII, 0,
                          "Zone 10N"));
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeProjected));
  ASSERT_EQ(1, GTIFSetVersionNumbers(handle.gtif.get(), 1, 1, 0));

  ASSERT_EQ(1, GTIFWriteKeys(handle.gtif.get()));
  EXPECT_EQ(0, handle.gtif->gt_flags & FLAG_FILE_MODIFIED);

  // Subsequent write without modifications returns 1 immediately.
  EXPECT_EQ(1, GTIFWriteKeys(handle.gtif.get()));

  // 2. Inspect serialized raw tags in ST_TIFF directly.
  int dir_count = 0;
  int dir_type = 0;
  void* dir_data = nullptr;
  ASSERT_EQ(1, ST_GetKey(handle.st.get(), TIFFTAG_GEOKEYDIRECTORY, &dir_count,
                         &dir_type, &dir_data));
  ASSERT_EQ(4 + 4 * 4, dir_count);
  const auto* header = static_cast<const KeyHeader*>(dir_data);
  EXPECT_EQ(1, header->hdr_version);
  EXPECT_EQ(1, header->hdr_rev_major);
  EXPECT_EQ(0, header->hdr_rev_minor);
  EXPECT_EQ(4, header->hdr_num_keys);

  const auto* entries = reinterpret_cast<const KeyEntry*>(
      static_cast<const pinfo_t*>(dir_data) + 4);
  EXPECT_EQ(GTModelTypeGeoKey, entries[0].ent_key);
  EXPECT_EQ(GTIFF_LOCAL, entries[0].ent_location);
  EXPECT_EQ(GTCitationGeoKey, entries[1].ent_key);
  EXPECT_EQ(TIFFTAG_GEOASCIIPARAMS, entries[1].ent_location);
  EXPECT_EQ(GeogSemiMajorAxisGeoKey, entries[2].ent_key);
  EXPECT_EQ(TIFFTAG_GEODOUBLEPARAMS, entries[2].ent_location);
  EXPECT_EQ(ProjectedCSTypeGeoKey, entries[3].ent_key);
  EXPECT_EQ(GTIFF_LOCAL, entries[3].ent_location);

  int ascii_count = 0;
  int ascii_type = 0;
  void* ascii_data = nullptr;
  ASSERT_EQ(1, ST_GetKey(handle.st.get(), TIFFTAG_GEOASCIIPARAMS, &ascii_count,
                         &ascii_type, &ascii_data));
  EXPECT_STREQ("Zone 10N|", static_cast<const char*>(ascii_data));

  int dbl_count = 0;
  int dbl_type = 0;
  void* dbl_data = nullptr;
  ASSERT_EQ(1, ST_GetKey(handle.st.get(), TIFFTAG_GEODOUBLEPARAMS, &dbl_count,
                         &dbl_type, &dbl_data));
  EXPECT_EQ(1, dbl_count);
  EXPECT_DOUBLE_EQ(6378137.0, static_cast<const double*>(dbl_data)[0]);
}

TEST(GeoWriteTest, WritesZeroKeysAfterDeleteWithoutDoubleOrAsciiTags) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeProjected));
  ASSERT_EQ(1,
            GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, -1));

  // gt_num_keys == 0 with FLAG_FILE_MODIFIED exercises SortKeys empty return
  // and omits optional DOUBLE/ASCII tags.
  ASSERT_EQ(1, GTIFWriteKeys(handle.gtif.get()));
  EXPECT_EQ(0, ST_GetKey(handle.st.get(), TIFFTAG_GEODOUBLEPARAMS, nullptr,
                         nullptr, nullptr));
  EXPECT_EQ(0, ST_GetKey(handle.st.get(), TIFFTAG_GEOASCIIPARAMS, nullptr,
                         nullptr, nullptr));
}

TEST(GeoWriteTest, ReWritesMultiValuedShortKeyLocatedInGeoKeyDirectory) {
  ScopedSimpleTags st(ST_Create());
  ASSERT_NE(nullptr, st);

  // Inject a valid multi-valued SHORT key whose values live at offset 12 inside
  // TIFFTAG_GEOKEYDIRECTORY (leaving [8..11] free for the second KeyEntry
  // written by GTIFWriteKeys), then trigger GTIFWriteKeys via another key.
  std::array<uint16_t, 14> raw_dir = {
      1,    1,
      0,    1,  // KeyHeader: 1 key
      9001, TIFFTAG_GEOKEYDIRECTORY,
      2,    12,  // KeyEntry pointing to offset 12
      0,    0,
      0,    0,    // Space for second KeyEntry
      111,  222,  // 2 short values at [12..13]
  };
  ASSERT_EQ(1, SetKeyDirectory(st.get(), raw_dir));

  ScopedGtif gtif(GTIFNewSimpleTags(st.get()));
  ASSERT_NE(nullptr, gtif);
  ASSERT_EQ(1, GTIFKeySet(gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeProjected));
  ASSERT_EQ(1, GTIFWriteKeys(gtif.get()));

  std::array<uint16_t, 2> read_shorts = {0, 0};
  EXPECT_EQ(2, GTIFKeyGetSHORT(gtif.get(), static_cast<geokey_t>(9001),
                               read_shorts.data(), 0, 2));
  EXPECT_EQ(111, read_shorts[0]);
  EXPECT_EQ(222, read_shorts[1]);
}

TEST(GeoWriteTest, WriteKeyUnknownTypeFailsAndFreesTempAsciiBuffer) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTCitationGeoKey, TYPE_ASCII, 0,
                          "Buffered Text"));
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeProjected));

  // Mutate the internal GeoKey type via geo_keyp.h to exercise WriteKey's
  // default failure branch and tk_asciiParams cleanup in GTIFWriteKeys.
  handle.gtif->gt_keys[2].gk_type = TYPE_UNKNOWN;
  EXPECT_EQ(0, GTIFWriteKeys(handle.gtif.get()));
}

// TODO(schwehr): Empty `!SortKeys(...)` block (geo_write.c:54) and
// `_GTIFcalloc` OOM / `tk_asciiParams == NULL` guards in GTIFWriteKeys and
// WriteKey (geo_write.c:77, 158) are not triggered without fault injection.

}  // namespace
}  // namespace libgeotiff
