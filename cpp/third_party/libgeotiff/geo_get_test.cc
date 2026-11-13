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
// Tests GeoKey header queries and typed/untyped key retrieval (geo_get.c).
//
// See also:
//   https://github.com/OSGeo/libgeotiff/blob/master/libgeotiff/geo_get.c

#include <array>
#include <cstdint>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/libgeotiff/geo_simpletags.h"
#include "third_party/libgeotiff/geokeys.h"
#include "third_party/libgeotiff/geotiff.h"
#include "third_party/libgeotiff/geovalues.h"
#include "third_party/libgeotiff/google/util/scoped_geotiff.h"

namespace libgeotiff {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

TEST(GeoGetTest, DirectoryInfoReportsVersionsAndKeyCounts) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  std::array<int, 3> versions = {-1, -1, -1};
  int keycount = -1;
  GTIFDirectoryInfo(handle.gtif.get(), versions.data(), &keycount);
  EXPECT_EQ(GvCurrentVersion, versions[0]);
  EXPECT_EQ(1, versions[1]);
  EXPECT_EQ(0, versions[2]);
  EXPECT_EQ(0, keycount);

  // Null optional out-parameters are safe no-ops.
  GTIFDirectoryInfo(handle.gtif.get(), nullptr, nullptr);

  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeProjected));
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTRasterTypeGeoKey, TYPE_SHORT, 1,
                          RasterPixelIsArea));
  GTIFDirectoryInfo(handle.gtif.get(), versions.data(), &keycount);
  EXPECT_EQ(2, keycount);
}

TEST(GeoGetTest, KeyInfoCoversMissingPopulatedAndNullOutParams) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  int size = -1;
  tagtype_t type = TYPE_UNKNOWN;
  EXPECT_EQ(0, GTIFKeyInfo(handle.gtif.get(), GTModelTypeGeoKey, &size, &type));

  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTCitationGeoKey, TYPE_ASCII, 0,
                          "WGS84"));
  EXPECT_EQ(6, GTIFKeyInfo(handle.gtif.get(), GTCitationGeoKey, &size, &type));
  EXPECT_EQ(1, size);
  EXPECT_EQ(TYPE_ASCII, type);

  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogSemiMajorAxisGeoKey,
                          TYPE_DOUBLE, 1, 6378137.0));
  EXPECT_EQ(
      1, GTIFKeyInfo(handle.gtif.get(), GeogSemiMajorAxisGeoKey, &size, &type));
  EXPECT_EQ(8, size);
  EXPECT_EQ(TYPE_DOUBLE, type);

  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeGeographic));
  EXPECT_EQ(1, GTIFKeyInfo(handle.gtif.get(), GTModelTypeGeoKey, &size, &type));
  EXPECT_EQ(2, size);
  EXPECT_EQ(TYPE_SHORT, type);
  EXPECT_EQ(
      1, GTIFKeyInfo(handle.gtif.get(), GTModelTypeGeoKey, nullptr, nullptr));
}

TEST(GeoGetTest, KeyGetEdgeCasesSlicingAndClamping) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  uint16_t short_val = 99;
  EXPECT_EQ(0,
            GTIFKeyGet(handle.gtif.get(), GTModelTypeGeoKey, &short_val, 0, 1));

  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeGeographic));
  EXPECT_EQ(1,
            GTIFKeyGet(handle.gtif.get(), GTModelTypeGeoKey, &short_val, 0, 1));
  EXPECT_EQ(ModelTypeGeographic, short_val);

  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTCitationGeoKey, TYPE_ASCII, 0,
                          "HelloGeo"));
  // 1. Out-of-bounds offset with count=0 yields count <= 0 and returns 0.
  std::array<char, 32> buf{};
  EXPECT_EQ(0,
            GTIFKeyGet(handle.gtif.get(), GTCitationGeoKey, buf.data(), 9, 0));

  // 2. count=0 computes remaining length including trailing NUL
  // (len("HelloGeo")+1 == 9).
  EXPECT_EQ(9,
            GTIFKeyGet(handle.gtif.get(), GTCitationGeoKey, buf.data(), 0, 0));
  EXPECT_STREQ("HelloGeo", buf.data());

  // 3. Partial read forces NUL in last slot (buf[count - 1] = '\0').
  std::array<char, 8> truncated;
  truncated.fill('x');
  EXPECT_EQ(4, GTIFKeyGet(handle.gtif.get(), GTCitationGeoKey, truncated.data(),
                          0, 4));
  EXPECT_STREQ("Hel", truncated.data());

  // 4. count > gk_count clamps to gk_count (9).
  EXPECT_EQ(9,
            GTIFKeyGet(handle.gtif.get(), GTCitationGeoKey, buf.data(), 0, 64));
  EXPECT_STREQ("HelloGeo", buf.data());

  // 5. Slice read at non-zero index on multi-element DOUBLE array.
  std::array<double, 2> raw_params = {12.5, -45.75};
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), ProjFalseEastingGeoKey,
                          TYPE_DOUBLE, 2, raw_params.data()));
  double second = 0.0;
  EXPECT_EQ(
      1, GTIFKeyGet(handle.gtif.get(), ProjFalseEastingGeoKey, &second, 1, 1));
  EXPECT_DOUBLE_EQ(-45.75, second);
  std::array<double, 2> read_params = {0.0, 0.0};
  EXPECT_EQ(2, GTIFKeyGet(handle.gtif.get(), ProjFalseEastingGeoKey,
                          read_params.data(), 0, 0));
  EXPECT_THAT(read_params, ElementsAre(12.5, -45.75));
}

TEST(GeoGetTest, TypedGettersSuccessMissingAndMismatchCallback) {
  ScopedSimpleTags st(ST_Create());
  ASSERT_NE(nullptr, st);
  ErrorCapture capture;
  ScopedGtif gtif = MakeCapturingSimpleTagsGtif(st.get(), &capture);
  ASSERT_NE(nullptr, gtif);

  // 1. Missing keys return 0 without emitting warnings.
  uint16_t u16 = 0;
  double dval = 0.0;
  std::array<char, 32> sbuf{};
  EXPECT_EQ(0, GTIFKeyGetSHORT(gtif.get(), GTModelTypeGeoKey, &u16, 0, 1));
  EXPECT_EQ(0,
            GTIFKeyGetDOUBLE(gtif.get(), GeogSemiMajorAxisGeoKey, &dval, 0, 1));
  EXPECT_EQ(0, GTIFKeyGetASCII(gtif.get(), GTCitationGeoKey, sbuf.data(),
                               static_cast<int>(sbuf.size())));
  EXPECT_TRUE(capture.records.empty());

  // 2. Populate keys of each type and verify happy paths.
  ASSERT_EQ(1, GTIFKeySet(gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeProjected));
  ASSERT_EQ(1, GTIFKeySet(gtif.get(), GeogSemiMajorAxisGeoKey, TYPE_DOUBLE, 1,
                          6378137.0));
  ASSERT_EQ(
      1, GTIFKeySet(gtif.get(), GTCitationGeoKey, TYPE_ASCII, 0, "Typed Test"));
  EXPECT_EQ(1, GTIFKeyGetSHORT(gtif.get(), GTModelTypeGeoKey, &u16, 0, 1));
  EXPECT_EQ(ModelTypeProjected, u16);
  EXPECT_EQ(1,
            GTIFKeyGetDOUBLE(gtif.get(), GeogSemiMajorAxisGeoKey, &dval, 0, 1));
  EXPECT_DOUBLE_EQ(6378137.0, dval);
  EXPECT_EQ(11, GTIFKeyGetASCII(gtif.get(), GTCitationGeoKey, sbuf.data(),
                                static_cast<int>(sbuf.size())));
  EXPECT_STREQ("Typed Test", sbuf.data());

  // 3. Mismatched types emit LIBGEOTIFF_WARNING via gt_error_callback.
  EXPECT_EQ(0, GTIFKeyGetSHORT(gtif.get(), GTCitationGeoKey, &u16, 0, 1));
  EXPECT_EQ(0, GTIFKeyGetDOUBLE(gtif.get(), GTModelTypeGeoKey, &dval, 0, 1));
  EXPECT_EQ(0, GTIFKeyGetASCII(gtif.get(), GeogSemiMajorAxisGeoKey, sbuf.data(),
                               static_cast<int>(sbuf.size())));
  EXPECT_EQ(gtif.get(), capture.last_gtif);
  ASSERT_EQ(3U, capture.records.size());
  EXPECT_EQ(LIBGEOTIFF_WARNING, capture.records[0].level);
  EXPECT_THAT(capture.records[0].message, HasSubstr("GTCitationGeoKey"));
  EXPECT_THAT(capture.records[0].message, HasSubstr("Short"));
  EXPECT_THAT(capture.records[0].message, HasSubstr("Ascii"));

  // 4. Mismatch with null error callback safely returns 0 without crashing.
  ScopedSimpleTags st_silent(ST_Create());
  ASSERT_NE(nullptr, st_silent);
  ScopedGtif silent_gtif =
      MakeCapturingSimpleTagsGtif(st_silent.get(), nullptr);
  ASSERT_NE(nullptr, silent_gtif);
  ASSERT_EQ(1, GTIFKeySet(silent_gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeGeographic));
  EXPECT_EQ(0, GTIFKeyGetASCII(silent_gtif.get(), GTModelTypeGeoKey,
                               sbuf.data(), static_cast<int>(sbuf.size())));
}

}  // namespace
}  // namespace libgeotiff
