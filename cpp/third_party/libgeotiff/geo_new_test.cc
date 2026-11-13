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
// Tests GeoTIFF handle constructors and ReadKey parsing paths (geo_new.c).
//
// See also:
//   https://github.com/OSGeo/libgeotiff/blob/master/libgeotiff/geo_new.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/libgeotiff/geo_simpletags.h"
#include "third_party/libgeotiff/geo_tiffp.h"
#include "third_party/libgeotiff/geokeys.h"
#include "third_party/libgeotiff/geotiff.h"
#include "third_party/libgeotiff/geovalues.h"
#include "third_party/libgeotiff/google/util/scoped_geotiff.h"
#include "third_party/libgeotiff/google/util/tiff_mem_stream.h"
#include "third_party/libgeotiff/libxtiff/xtiffio.h"  // IWYU pragma: keep

namespace libgeotiff {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::SizeIs;

TEST(GeoNewTest, NullAndEmptyLibtiffStreamsCreateBlankHandles) {
  // 1. GTIFNew(nullptr) creates a blank handle with default methods.
  ScopedGtif null_gtif(GTIFNew(nullptr));
  ASSERT_NE(nullptr, null_gtif);
  EXPECT_EQ(nullptr, GTIFGetUserData(null_gtif.get()));
  std::array<int, 3> version = {0, 0, 0};
  int key_count = -1;
  GTIFDirectoryInfo(null_gtif.get(), version.data(), &key_count);
  EXPECT_EQ(GvCurrentVersion, version[0]);
  EXPECT_EQ(0, key_count);

  // 2. GTIFNewEx on an in-memory baseline TIFF without GeoTIFF tags.
  MemoryTiffStream stream;
  {
    ScopedTiff tif = stream.Open("w");
    ASSERT_NE(nullptr, tif);
    ASSERT_TRUE(WriteMinimalBaselineImage(tif.get()));
    ASSERT_EQ(1, TIFFWriteDirectory(tif.get()));
  }
  ScopedTiff read_tif = stream.Open("r");
  ASSERT_NE(nullptr, read_tif);
  ErrorCapture capture;
  ScopedGtif empty_tif_gtif(
      GTIFNewEx(read_tif.get(), CaptureGtifError, &capture));
  ASSERT_NE(nullptr, empty_tif_gtif);
  EXPECT_EQ(&capture, GTIFGetUserData(empty_tif_gtif.get()));
  EXPECT_THAT(capture.records, IsEmpty());
  GTIFDirectoryInfo(empty_tif_gtif.get(), nullptr, &key_count);
  EXPECT_EQ(0, key_count);
}

TEST(GeoNewTest, ReadsAllKeyTypesFromInMemoryLibtiffRoundTrip) {
  MemoryTiffStream stream;
  {
    ScopedTiff tif = stream.Open("w");
    ASSERT_NE(nullptr, tif);
    ASSERT_TRUE(WriteMinimalBaselineImage(tif.get()));

    ScopedGtif writer(GTIFNew(tif.get()));
    ASSERT_NE(nullptr, writer);
    ASSERT_EQ(1, GTIFKeySet(writer.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                            ModelTypeGeographic));
    ASSERT_EQ(1, GTIFKeySet(writer.get(), GeogCitationGeoKey, TYPE_ASCII, 0,
                            "WGS 84"));
    ASSERT_EQ(1, GTIFKeySet(writer.get(), GeogSemiMajorAxisGeoKey, TYPE_DOUBLE,
                            1, 6378137.0));
    ASSERT_EQ(1, GTIFWriteKeys(writer.get()));
    ASSERT_EQ(1, TIFFWriteDirectory(tif.get()));
  }

  ScopedTiff read_tif = stream.Open("r");
  ASSERT_NE(nullptr, read_tif);
  ScopedGtif reader(GTIFNew(read_tif.get()));
  ASSERT_NE(nullptr, reader);

  uint16_t model = 0;
  std::array<char, 32> citation{};
  double semi_major = 0.0;
  EXPECT_EQ(1, GTIFKeyGetSHORT(reader.get(), GTModelTypeGeoKey, &model, 0, 1));
  EXPECT_EQ(ModelTypeGeographic, model);
  EXPECT_EQ(7,
            GTIFKeyGetASCII(reader.get(), GeogCitationGeoKey, citation.data(),
                            static_cast<int>(citation.size())));
  EXPECT_STREQ("WGS 84", citation.data());
  EXPECT_EQ(1, GTIFKeyGetDOUBLE(reader.get(), GeogSemiMajorAxisGeoKey,
                                &semi_major, 0, 1));
  EXPECT_DOUBLE_EQ(6378137.0, semi_major);
}

TEST(GeoNewTest, NewWithMethodsHandlesNoNulAsciiAndMultiValuedShort) {
  struct CustomTags {
    std::vector<pinfo_t> dir;
    std::string ascii_no_nul;
  } custom;

  custom.ascii_no_nul = "Custom|";
  custom.dir = {
      1,
      1,
      0,
      2,
      // Key 1: ASCII without trailing NUL in tag bytes.
      1026,
      TIFFTAG_GEOASCIIPARAMS,
      7,
      0,
      // Key 2: multi-valued SHORT inside directory starting at offset 12.
      9002,
      TIFFTAG_GEOKEYDIRECTORY,
      2,
      12,
      77,
      88,
  };

  TIFFMethod methods{};
  methods.get = [](tiff_t* tif, pinfo_t tag, int* count, void* val) -> int {
    auto* tags = reinterpret_cast<CustomTags*>(tif);
    if (tag == GTIFF_GEOKEYDIRECTORY) {
      *count = static_cast<int>(tags->dir.size());
      auto* buf = static_cast<pinfo_t*>(
          _GTIFcalloc(tags->dir.size() * sizeof(pinfo_t)));
      std::memcpy(buf, tags->dir.data(), tags->dir.size() * sizeof(pinfo_t));
      *static_cast<void**>(val) = buf;
      return 1;
    }
    if (tag == GTIFF_ASCIIPARAMS) {
      *count = static_cast<int>(tags->ascii_no_nul.size());
      auto* buf = static_cast<char*>(_GTIFcalloc(tags->ascii_no_nul.size()));
      std::memcpy(buf, tags->ascii_no_nul.data(), tags->ascii_no_nul.size());
      *static_cast<void**>(val) = buf;
      return 1;
    }
    return 0;
  };
  methods.set = [](tiff_t*, pinfo_t, int, void*) -> int { return 1; };
  methods.type = [](tiff_t*, pinfo_t tag) -> tagtype_t {
    if (tag == GTIFF_ASCIIPARAMS) return TYPE_ASCII;
    if (tag == GTIFF_GEOKEYDIRECTORY) return TYPE_SHORT;
    return TYPE_UNKNOWN;
  };

  ScopedGtif gtif(GTIFNewWithMethods(&custom, &methods));
  ASSERT_NE(nullptr, gtif);
  EXPECT_EQ(nullptr, GTIFGetUserData(gtif.get()));

  std::array<char, 16> ascii{};
  EXPECT_EQ(7, GTIFKeyGetASCII(gtif.get(), GTCitationGeoKey, ascii.data(),
                               static_cast<int>(ascii.size())));
  EXPECT_STREQ("Custom", ascii.data());

  std::array<uint16_t, 2> shorts{};
  EXPECT_EQ(2, GTIFKeyGetSHORT(gtif.get(), static_cast<geokey_t>(9002),
                               shorts.data(), 0, 2));
  EXPECT_EQ(77, shorts[0]);
  EXPECT_EQ(88, shorts[1]);
}

TEST(GeoNewTest, ValidatesHeaderVersionsAndTagArrayBounds) {
  constexpr size_t kMaxValues = 1000;  // MAX_VALUES in geo_keyp.h.

  // 1. Future major revision emits no error and succeeds.
  {
    ScopedSimpleTags st(ST_Create());
    std::array<uint16_t, 4> dir = {GvCurrentVersion, GvCurrentRevision + 1, 0,
                                   0};
    ASSERT_EQ(1, SetKeyDirectory(st.get(), dir));
    ScopedGtif gtif(GTIFNewSimpleTags(st.get()));
    EXPECT_NE(nullptr, gtif);
  }

  // 2. Unsupported header version (> GvCurrentVersion) fails.
  {
    ScopedSimpleTags st(ST_Create());
    std::array<uint16_t, 4> dir = {GvCurrentVersion + 1, 1, 0, 0};
    ASSERT_EQ(1, SetKeyDirectory(st.get(), dir));
    EXPECT_EQ(nullptr, GTIFNewSimpleTags(st.get()));
  }

  // 3. Key count exceeding directory buffer capacity fails:
  // 251 * sizeof(KeyEntry) (2008 bytes) >= (4 + MAX_VALUES) * sizeof(pinfo_t).
  {
    ScopedSimpleTags st(ST_Create());
    std::array<uint16_t, 4> dir = {1, 1, 0, 251};
    ASSERT_EQ(1, SetKeyDirectory(st.get(), dir));
    EXPECT_EQ(nullptr, GTIFNewSimpleTags(st.get()));
  }

  // 4. GeoDoubleParams exceeding MAX_VALUES (1000) fails.
  {
    ScopedSimpleTags st(ST_Create());
    std::array<uint16_t, 4> dir = {1, 1, 0, 0};
    std::vector<double> too_many(kMaxValues + 1, 1.0);
    ASSERT_EQ(1, SetKeyDirectory(st.get(), dir));
    ASSERT_EQ(1, SetSimpleTag(st.get(), TIFFTAG_GEODOUBLEPARAMS, STT_DOUBLE,
                              too_many));
    EXPECT_EQ(nullptr, GTIFNewSimpleTags(st.get()));
  }
}

TEST(GeoNewTest, HandlesAsciiVendorNullTruncationAndErrorBranches) {
  // 1. Vendor-style ASCII without '|', relying on trailing NUL stripped during
  // header load: count includes the stripped NUL so ReadKey decrements count.
  {
    ScopedSimpleTags st(ST_Create());
    std::array<uint16_t, 8> dir = {
        1, 1, 0, 1, GTCitationGeoKey, TIFFTAG_GEOASCIIPARAMS, 7, 0,
    };
    std::string vendor_ascii = "Vendor";
    ASSERT_EQ(1, SetKeyDirectory(st.get(), dir));
    ASSERT_EQ(1, ST_SetKey(st.get(), TIFFTAG_GEOASCIIPARAMS, 0, STT_ASCII,
                           vendor_ascii.data()));

    ErrorCapture capture;
    ScopedGtif gtif = MakeCapturingSimpleTagsGtif(st.get(), &capture);
    ASSERT_NE(nullptr, gtif);
    EXPECT_THAT(capture.records, IsEmpty());

    std::array<char, 16> out{};
    EXPECT_EQ(7, GTIFKeyGetASCII(gtif.get(), GTCitationGeoKey, out.data(),
                                 static_cast<int>(out.size())));
    EXPECT_STREQ("Vendor", out.data());
  }

  // 2. Partial out-of-bounds ASCII triggers warning and truncates value
  // (also called via GTIFNewSimpleTags to cover default GTIFErrorFunction
  // stderr output).
  {
    ScopedSimpleTags st(ST_Create());
    std::array<uint16_t, 8> dir = {
        1, 1, 0, 1, GTCitationGeoKey, TIFFTAG_GEOASCIIPARAMS, 10, 0,
    };
    std::string short_ascii = "Short|";
    ASSERT_EQ(1, SetKeyDirectory(st.get(), dir));
    ASSERT_EQ(1, ST_SetKey(st.get(), TIFFTAG_GEOASCIIPARAMS, 0, STT_ASCII,
                           short_ascii.data()));

    // First via GTIFNewSimpleTags to cover GTIFErrorFunction warning branch.
    ScopedGtif default_err_gtif(GTIFNewSimpleTags(st.get()));
    ASSERT_NE(nullptr, default_err_gtif);

    // Then via CaptureGtifError to inspect the warning details.
    ErrorCapture capture;
    ScopedGtif gtif = MakeCapturingSimpleTagsGtif(st.get(), &capture);
    ASSERT_NE(nullptr, gtif);
    EXPECT_EQ(gtif.get(), capture.last_gtif);
    ASSERT_THAT(capture.records, SizeIs(1));
    EXPECT_EQ(LIBGEOTIFF_WARNING, capture.records[0].level);
    EXPECT_THAT(capture.records[0].message, HasSubstr("Truncating"));

    std::array<char, 16> out{};
    EXPECT_EQ(6, GTIFKeyGetASCII(gtif.get(), GTCitationGeoKey, out.data(),
                                 static_cast<int>(out.size())));
    EXPECT_STREQ("Short", out.data());
  }

  // 3. Completely out-of-bounds ASCII fails with error.
  {
    ScopedSimpleTags st(ST_Create());
    std::array<uint16_t, 8> dir = {
        1, 1, 0, 1, GTCitationGeoKey, TIFFTAG_GEOASCIIPARAMS, 4, 10,
    };
    std::string short_ascii = "Short|";
    ASSERT_EQ(1, SetKeyDirectory(st.get(), dir));
    ASSERT_EQ(1, ST_SetKey(st.get(), TIFFTAG_GEOASCIIPARAMS, 0, STT_ASCII,
                           short_ascii.data()));

    ErrorCapture capture;
    EXPECT_EQ(nullptr, MakeCapturingSimpleTagsGtif(st.get(), &capture));
    ASSERT_THAT(capture.records, SizeIs(1));
    EXPECT_EQ(LIBGEOTIFF_ERROR, capture.records[0].level);
    EXPECT_THAT(capture.records[0].message, HasSubstr("GeoAsciiParams"));
  }

  // 4. Missing GeoAsciiParams when key uses TIFFTAG_GEOASCIIPARAMS fails.
  {
    ScopedSimpleTags st(ST_Create());
    std::array<uint16_t, 8> dir = {
        1, 1, 0, 1, GTCitationGeoKey, TIFFTAG_GEOASCIIPARAMS, 5, 0,
    };
    ASSERT_EQ(1, SetKeyDirectory(st.get(), dir));

    ErrorCapture capture;
    EXPECT_EQ(nullptr, MakeCapturingSimpleTagsGtif(st.get(), &capture));
    ASSERT_THAT(capture.records, SizeIs(1));
    EXPECT_EQ(LIBGEOTIFF_ERROR, capture.records[0].level);
    EXPECT_THAT(capture.records[0].message, HasSubstr("missing or corrupted"));
  }
}

TEST(GeoNewTest, RejectsInvalidLocalShortDoubleAndUnknownLocations) {
  // 1. GTIFF_LOCAL with count != 1 fails (tested with default GTIFErrorFunction
  // as well to exercise expected LIBGEOTIFF_ERROR stderr formatting).
  {
    ScopedSimpleTags st(ST_Create());
    std::array<uint16_t, 8> dir = {1, 1, 0, 1, GTModelTypeGeoKey, 0, 2, 1};
    ASSERT_EQ(1, SetKeyDirectory(st.get(), dir));
    EXPECT_EQ(nullptr, GTIFNewSimpleTags(st.get()));

    ErrorCapture capture;
    EXPECT_EQ(nullptr, MakeCapturingSimpleTagsGtif(st.get(), &capture));
    ASSERT_THAT(capture.records, SizeIs(1));
    EXPECT_EQ(LIBGEOTIFF_ERROR, capture.records[0].level);
    EXPECT_THAT(capture.records[0].message, HasSubstr("only 1 is legal"));
  }

  // 2. GTIFF_GEOKEYDIRECTORY offset/count exceeding gt_nshorts fails.
  {
    ScopedSimpleTags st(ST_Create());
    std::array<uint16_t, 8> dir = {
        1, 1, 0, 1, 9001, TIFFTAG_GEOKEYDIRECTORY, 3, 8,
    };
    ASSERT_EQ(1, SetKeyDirectory(st.get(), dir));
    ErrorCapture capture;
    EXPECT_EQ(nullptr, MakeCapturingSimpleTagsGtif(st.get(), &capture));
    ASSERT_THAT(capture.records, SizeIs(1));
    EXPECT_THAT(capture.records[0].message, HasSubstr("GeoKeyDirectory"));
  }

  // 3. GTIFF_DOUBLEPARAMS offset/count exceeding gt_ndoubles fails.
  {
    ScopedSimpleTags st(ST_Create());
    std::array<uint16_t, 8> dir = {
        1, 1, 0, 1, GeogSemiMajorAxisGeoKey, TIFFTAG_GEODOUBLEPARAMS, 2, 0,
    };
    std::array<double, 1> single_val = {6378137.0};
    ASSERT_EQ(1, SetKeyDirectory(st.get(), dir));
    ASSERT_EQ(1, SetSimpleTag(st.get(), TIFFTAG_GEODOUBLEPARAMS, STT_DOUBLE,
                              single_val));
    ErrorCapture capture;
    EXPECT_EQ(nullptr, MakeCapturingSimpleTagsGtif(st.get(), &capture));
    ASSERT_THAT(capture.records, SizeIs(1));
    EXPECT_THAT(capture.records[0].message, HasSubstr("GeoDoubleParams"));
  }

  // 4. Unknown TIFFTagLocation fails.
  {
    ScopedSimpleTags st(ST_Create());
    std::array<uint16_t, 8> dir = {
        1, 1, 0, 1, GTModelTypeGeoKey, 12345, 1, 0,
    };
    ASSERT_EQ(1, SetKeyDirectory(st.get(), dir));
    ErrorCapture capture;
    EXPECT_EQ(nullptr, MakeCapturingSimpleTagsGtif(st.get(), &capture));
    ASSERT_THAT(capture.records, SizeIs(1));
    EXPECT_THAT(capture.records[0].message, HasSubstr("unknown type"));
  }
}

// TODO(schwehr): Simulate malloc OOM checks in GTIFNewWithMethodsEx
// (geo_new.c:116, 132, 172, 203, 205) and verify null gt_error_callback guards
// with fault injection.

}  // namespace
}  // namespace libgeotiff
