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
// Tests GeoKey creation, overwrite, deletion, limits, and directory version
// metadata (geo_set.c).
//
// See also:
//   https://github.com/OSGeo/libgeotiff/blob/master/libgeotiff/geo_set.c

#include <array>
#include <cstdint>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/libgeotiff/geokeys.h"
#include "third_party/libgeotiff/geotiff.h"
#include "third_party/libgeotiff/geovalues.h"
#include "third_party/libgeotiff/google/util/scoped_geotiff.h"

namespace libgeotiff {
namespace {

using ::testing::ElementsAre;

TEST(GeoSetTest, SetAndOverwriteShortAsciiAndDoubleKeys) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  // 1. Set initial SHORT, ASCII, and DOUBLE keys (always pass double for
  // TYPE_DOUBLE, count=1 variadic arguments).
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeProjected));
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTCitationGeoKey, TYPE_ASCII, 0,
                          "Initial"));
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogSemiMajorAxisGeoKey,
                          TYPE_DOUBLE, 1, 6378137.0));

  // 2. Overwrite SHORT in place.
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeGeographic));
  uint16_t model_type = 0;
  EXPECT_EQ(1, GTIFKeyGetSHORT(handle.gtif.get(), GTModelTypeGeoKey,
                               &model_type, 0, 1));
  EXPECT_EQ(ModelTypeGeographic, model_type);

  // 3. Overwrite ASCII with longer value (frees prior buffer and grows count).
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTCitationGeoKey, TYPE_ASCII, 0,
                          "Longer Citation String"));
  std::array<char, 64> citation{};
  EXPECT_EQ(
      23, GTIFKeyGetASCII(handle.gtif.get(), GTCitationGeoKey, citation.data(),
                          static_cast<int>(citation.size())));
  EXPECT_STREQ("Longer Citation String", citation.data());

  // 4. Overwrite existing SHORT key with TYPE_DOUBLE (exercises type-change
  // branch cleanly without leaving an unfreed ASCII buffer).
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTRasterTypeGeoKey, TYPE_SHORT, 1,
                          RasterPixelIsArea));
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTRasterTypeGeoKey, TYPE_DOUBLE, 1,
                          1.5));
  double changed_double = 0.0;
  EXPECT_EQ(1, GTIFKeyGetDOUBLE(handle.gtif.get(), GTRasterTypeGeoKey,
                                &changed_double, 0, 1));
  EXPECT_DOUBLE_EQ(1.5, changed_double);

  // 5. Overwrite singleton DOUBLE with multi-valued DOUBLE array.
  std::array<double, 3> multi_double = {10.0, 20.0, 30.0};
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogSemiMajorAxisGeoKey,
                          TYPE_DOUBLE, 3, multi_double.data()));
  std::array<double, 3> out_double = {0.0, 0.0, 0.0};
  EXPECT_EQ(3, GTIFKeyGetDOUBLE(handle.gtif.get(), GeogSemiMajorAxisGeoKey,
                                out_double.data(), 0, 3));
  EXPECT_THAT(out_double, ElementsAre(10.0, 20.0, 30.0));
}

TEST(GeoSetTest, DeleteKeysShiftsEntriesAndFreesAscii) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  // 1. Deleting an unset key returns 0.
  EXPECT_EQ(0,
            GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, -1));

  // 2. Populate three keys, then delete the first (ASCII) and last (DOUBLE).
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTCitationGeoKey, TYPE_ASCII, 0,
                          "To Delete"));
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeProjected));
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogSemiMajorAxisGeoKey,
                          TYPE_DOUBLE, 1, 6378137.0));

  EXPECT_EQ(1, GTIFKeySet(handle.gtif.get(), GTCitationGeoKey, TYPE_ASCII, -1));
  EXPECT_EQ(0,
            GTIFKeyInfo(handle.gtif.get(), GTCitationGeoKey, nullptr, nullptr));

  int keycount = -1;
  GTIFDirectoryInfo(handle.gtif.get(), nullptr, &keycount);
  EXPECT_EQ(2, keycount);

  // 3. Remaining keys stay intact and indexed properly.
  uint16_t model = 0;
  EXPECT_EQ(
      1, GTIFKeyGetSHORT(handle.gtif.get(), GTModelTypeGeoKey, &model, 0, 1));
  EXPECT_EQ(ModelTypeProjected, model);

  EXPECT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogSemiMajorAxisGeoKey,
                          TYPE_DOUBLE, -1));
  GTIFDirectoryInfo(handle.gtif.get(), nullptr, &keycount);
  EXPECT_EQ(1, keycount);
}

TEST(GeoSetTest, RejectsMultiValuedShortAndUnknownArrayType) {
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    std::array<uint16_t, 2> shorts = {1, 2};
    EXPECT_EQ(0, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 2,
                            shorts.data()));
  }

  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    // Passing count > 1 avoids the count <= 1 assert(FALSE) abort at
    // geo_set.c:132 while exercising the unsupported-type return 0 path.
    std::array<uint32_t, 2> longs = {10, 20};
    EXPECT_EQ(0, GTIFKeySet(handle.gtif.get(), ProjectedCSTypeGeoKey, TYPE_LONG,
                            2, longs.data()));
  }
}

TEST(GeoSetTest, EnforcesMaxKeysLimit) {
  // Seed 1 key on disk/tags first because geo_new.c allocates
  // `bufcount = count + MAX_KEYS` slots while `geo_set.c` uses 1-based indexing
  // (`gt_keys[1..MAX_KEYS]`): with `count == 0`, slot `100` overflows by 1.
  // With `count == 1`, `bufcount == 101`, so slots `1..100` fit cleanly and the
  // 101st distinct key hits `gt_num_keys == MAX_KEYS` -> 0 without overflow.
  SimpleTagsGtif seed = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, seed.gtif);
  ASSERT_EQ(1, GTIFKeySet(seed.gtif.get(), static_cast<geokey_t>(10000),
                          TYPE_SHORT, 1, 0));
  ASSERT_EQ(1, GTIFWriteKeys(seed.gtif.get()));
  seed.gtif.reset();

  ScopedGtif gtif(GTIFNewSimpleTags(seed.st.get()));
  ASSERT_NE(nullptr, gtif);

  constexpr int kMaxKeys = 100;  // MAX_KEYS in geo_keyp.h.
  // 1 key was seeded above; add keys 1..kMaxKeys-1 via TYPE_SHORT.
  for (int i = 1; i < kMaxKeys; ++i) {
    const auto key = static_cast<geokey_t>(10000 + i);
    ASSERT_EQ(1, GTIFKeySet(gtif.get(), key, TYPE_SHORT, 1, i))
        << "key index=" << i;
  }
  EXPECT_EQ(0, GTIFKeySet(gtif.get(), static_cast<geokey_t>(10100), TYPE_SHORT,
                          1, 1));
}

TEST(GeoSetTest, SetVersionNumbersUpdatesDirectoryHeader) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  ASSERT_EQ(1,
            GTIFSetVersionNumbers(handle.gtif.get(), GEOTIFF_SPEC_1_1_VERSION,
                                  GEOTIFF_SPEC_1_1_KEY_REVISION,
                                  GEOTIFF_SPEC_1_1_MINOR_REVISION));
  std::array<int, 3> versions = {0, 0, 0};
  GTIFDirectoryInfo(handle.gtif.get(), versions.data(), nullptr);
  EXPECT_THAT(versions, ElementsAre(1, 1, 1));
}

// TODO(schwehr): `assert(FALSE)` for unsupported tagtype_t with count <= 1 in
// GTIFKeySet (geo_set.c:132) intentionally aborts when assertions are enabled
// and is omitted from normal sanitizer runs.

}  // namespace
}  // namespace libgeotiff
