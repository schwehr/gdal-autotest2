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
// Tests libxtiff extended GeoTIFF tag registration and open/close wrappers
// (libxtiff/xtiff.c) using //third_party/libgeotiff:xtiff without :libgeotiff.
//
// See also:
//   https://github.com/OSGeo/libgeotiff/blob/master/libgeotiff/libxtiff/xtiff.c

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <filesystem>  // NOLINT(build/c++17)
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/libgeotiff/google/util/tiff_mem_stream.h"
#include "third_party/libgeotiff/libxtiff/xtiffio.h"  // IWYU pragma: keep

namespace libgeotiff {
namespace {

using ::testing::IsEmpty;
using ::testing::Not;

static_assert(TIFFLIB_VERSION > 20220520,
              "HAVE_TIFFClientOpenExt expected in google3 libtiff >= 4.5.0");
#ifdef JPL_TAG_SUPPORT
#error "Update xtiff_test.cc to cover TIFFTAG_JPL_CARTO_IFD"
#endif

void WriteAllExtendedGeoTags(TIFF* tif) {
  ASSERT_TRUE(WriteMinimalBaselineImage(tif));

  std::array<double, 3> pixel_scale = {10.0, 10.0, 0.0};
  std::array<double, 16> intergraph_matrix = {
      1.0, 0.0, 0.0, 100.0, 0.0, 1.0, 0.0, 200.0,
      0.0, 0.0, 1.0, 0.0,   0.0, 0.0, 0.0, 1.0,
  };
  std::array<double, 16> trans_matrix = {
      2.0, 0.0, 0.0, 300.0, 0.0, -2.0, 0.0, 400.0,
      0.0, 0.0, 1.0, 0.0,   0.0, 0.0,  0.0, 1.0,
  };
  std::array<double, 6> tiepoints = {0.0, 0.0, 0.0, 500000.0, 4000000.0, 0.0};
  std::array<uint16_t, 8> key_dir = {1, 1, 0, 1, 1024, 0, 1, 1};
  std::array<double, 2> double_params = {6378137.0, 298.257223563};

  EXPECT_EQ(1, TIFFSetField(tif, TIFFTAG_GEOPIXELSCALE,
                            static_cast<uint16_t>(pixel_scale.size()),
                            pixel_scale.data()));
  EXPECT_EQ(1, TIFFSetField(tif, TIFFTAG_INTERGRAPH_MATRIX,
                            static_cast<uint16_t>(intergraph_matrix.size()),
                            intergraph_matrix.data()));
  EXPECT_EQ(1, TIFFSetField(tif, TIFFTAG_GEOTRANSMATRIX,
                            static_cast<uint16_t>(trans_matrix.size()),
                            trans_matrix.data()));
  EXPECT_EQ(1, TIFFSetField(tif, TIFFTAG_GEOTIEPOINTS,
                            static_cast<uint16_t>(tiepoints.size()),
                            tiepoints.data()));
  EXPECT_EQ(
      1, TIFFSetField(tif, TIFFTAG_GEOKEYDIRECTORY,
                      static_cast<uint16_t>(key_dir.size()), key_dir.data()));
  EXPECT_EQ(1, TIFFSetField(tif, TIFFTAG_GEODOUBLEPARAMS,
                            static_cast<uint16_t>(double_params.size()),
                            double_params.data()));
  EXPECT_EQ(1, TIFFSetField(tif, TIFFTAG_GEOASCIIPARAMS, "WGS84|"));
  ASSERT_EQ(1, TIFFWriteDirectory(tif));
}

void VerifyAllExtendedGeoTags(TIFF* tif) {
  uint16_t count = 0;
  double* doubles = nullptr;
  uint16_t* shorts = nullptr;
  char* ascii = nullptr;

  ASSERT_EQ(1, TIFFGetField(tif, TIFFTAG_GEOPIXELSCALE, &count, &doubles));
  EXPECT_EQ(3, count);
  EXPECT_DOUBLE_EQ(10.0, doubles[0]);

  ASSERT_EQ(1, TIFFGetField(tif, TIFFTAG_INTERGRAPH_MATRIX, &count, &doubles));
  EXPECT_EQ(16, count);
  EXPECT_DOUBLE_EQ(100.0, doubles[3]);

  ASSERT_EQ(1, TIFFGetField(tif, TIFFTAG_GEOTRANSMATRIX, &count, &doubles));
  EXPECT_EQ(16, count);
  EXPECT_DOUBLE_EQ(300.0, doubles[3]);

  ASSERT_EQ(1, TIFFGetField(tif, TIFFTAG_GEOTIEPOINTS, &count, &doubles));
  EXPECT_EQ(6, count);
  EXPECT_DOUBLE_EQ(500000.0, doubles[3]);

  ASSERT_EQ(1, TIFFGetField(tif, TIFFTAG_GEOKEYDIRECTORY, &count, &shorts));
  EXPECT_EQ(8, count);
  EXPECT_EQ(1024, shorts[4]);

  ASSERT_EQ(1, TIFFGetField(tif, TIFFTAG_GEODOUBLEPARAMS, &count, &doubles));
  EXPECT_EQ(2, count);
  EXPECT_DOUBLE_EQ(6378137.0, doubles[0]);

  ASSERT_EQ(1, TIFFGetField(tif, TIFFTAG_GEOASCIIPARAMS, &ascii));
  EXPECT_STREQ("WGS84|", ascii);
}

TEST(XtiffTest, InitializeIdempotentAndClientOpenRoundTripsAllTags) {
  XTIFFInitialize();
  XTIFFInitialize();

  // 1. XTIFFClientOpen round-trip via in-memory stream.
  {
    MemoryTiffStream stream;
    {
      ScopedTiff tif = stream.Open("w");
      ASSERT_NE(nullptr, tif);
      ASSERT_NO_FATAL_FAILURE(WriteAllExtendedGeoTags(tif.get()));
    }
    ScopedTiff read_tif = stream.Open("r");
    ASSERT_NE(nullptr, read_tif);
    ASSERT_NO_FATAL_FAILURE(VerifyAllExtendedGeoTags(read_tif.get()));

    // Empty read stream fails cleanly and returns nullptr.
    MemoryTiffStream empty;
    EXPECT_EQ(nullptr, empty.Open("r"));
  }

  // 2. XTIFFClientOpenExt round-trip with TIFFOpenOptions error capture.
  {
    TiffErrorCapture capture;
    ScopedTiffOpenOptions opts = MakeTiffOpenOptions(&capture);
    ASSERT_NE(nullptr, opts);

    MemoryTiffStream stream;
    {
      ScopedTiff tif = stream.OpenExt("w", opts.get());
      ASSERT_NE(nullptr, tif);
      ASSERT_NO_FATAL_FAILURE(WriteAllExtendedGeoTags(tif.get()));
    }
    ScopedTiff read_tif = stream.OpenExt("r", opts.get());
    ASSERT_NE(nullptr, read_tif);
    ASSERT_NO_FATAL_FAILURE(VerifyAllExtendedGeoTags(read_tif.get()));
    EXPECT_THAT(capture.errors, IsEmpty());
    EXPECT_THAT(capture.warnings, IsEmpty());

    MemoryTiffStream empty;
    EXPECT_EQ(nullptr, empty.OpenExt("r", opts.get()));
    ASSERT_THAT(capture.errors, Not(IsEmpty()));
    EXPECT_THAT(capture.errors[0].module, Not(IsEmpty()));
  }
}

TEST(XtiffTest, OpenAndOpenExtHandleTempFilesAndMissingPaths) {
  const std::filesystem::path tif_path =
      std::filesystem::path(::testing::TempDir()) / "xtiff_open_test.tif";
  const std::filesystem::path missing_path =
      std::filesystem::path(::testing::TempDir()) / "xtiff_missing.tif";
  absl::Cleanup remove_file = [&] { std::filesystem::remove(tif_path); };

  // 1. XTIFFOpen write + read + failure on missing file (default libtiff error
  // handler intentionally emits expected stderr diagnostics).
  {
    ScopedTiff writer(XTIFFOpen(tif_path.c_str(), "w"));
    ASSERT_NE(nullptr, writer);
    ASSERT_NO_FATAL_FAILURE(WriteAllExtendedGeoTags(writer.get()));
  }
  {
    ScopedTiff reader(XTIFFOpen(tif_path.c_str(), "r"));
    ASSERT_NE(nullptr, reader);
    ASSERT_NO_FATAL_FAILURE(VerifyAllExtendedGeoTags(reader.get()));
  }
  EXPECT_EQ(nullptr, XTIFFOpen(missing_path.c_str(), "r"));

  // 2. XTIFFOpenExt write + read + failure with error capture.
  TiffErrorCapture capture;
  ScopedTiffOpenOptions opts = MakeTiffOpenOptions(&capture);
  ASSERT_NE(nullptr, opts);
  {
    ScopedTiff writer(XTIFFOpenExt(tif_path.c_str(), "w", opts.get()));
    ASSERT_NE(nullptr, writer);
    ASSERT_NO_FATAL_FAILURE(WriteAllExtendedGeoTags(writer.get()));
  }
  {
    ScopedTiff reader(XTIFFOpenExt(tif_path.c_str(), "r", opts.get()));
    ASSERT_NE(nullptr, reader);
    ASSERT_NO_FATAL_FAILURE(VerifyAllExtendedGeoTags(reader.get()));
    EXPECT_THAT(capture.errors, IsEmpty());
    EXPECT_THAT(capture.warnings, IsEmpty());
  }
  EXPECT_EQ(nullptr, XTIFFOpenExt(missing_path.c_str(), "r", opts.get()));
  ASSERT_THAT(capture.errors, Not(IsEmpty()));
  EXPECT_THAT(capture.errors[0].module, Not(IsEmpty()));
}

TEST(XtiffTest, FdOpenAndFdOpenExtHandleValidAndInvalidDescriptors) {
  const std::filesystem::path tif_path =
      std::filesystem::path(::testing::TempDir()) / "xtiff_fd_test.tif";
  const std::filesystem::path empty_path =
      std::filesystem::path(::testing::TempDir()) / "xtiff_empty_fd.tif";
  absl::Cleanup remove_files = [&] {
    std::filesystem::remove(tif_path);
    std::filesystem::remove(empty_path);
  };

  // XTIFFFdOpen* takes ownership of `fd` only on success; cancel the Cleanup
  // guard once ownership transfers so XTIFFClose closes `fd` without double
  // close, and let it close `fd` on failure.
  // 1. XTIFFFdOpen write + read.
  {
    int fd = open(tif_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    absl::Cleanup close_fd = [fd] { close(fd); };
    ScopedTiff writer(XTIFFFdOpen(fd, tif_path.c_str(), "w"));
    ASSERT_NE(nullptr, writer);
    std::move(close_fd).Cancel();
    ASSERT_NO_FATAL_FAILURE(WriteAllExtendedGeoTags(writer.get()));
  }
  {
    int fd = open(tif_path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    absl::Cleanup close_fd = [fd] { close(fd); };
    ScopedTiff reader(XTIFFFdOpen(fd, tif_path.c_str(), "r"));
    ASSERT_NE(nullptr, reader);
    std::move(close_fd).Cancel();
    ASSERT_NO_FATAL_FAILURE(VerifyAllExtendedGeoTags(reader.get()));
  }

  // 2. XTIFFFdOpenExt write + read.
  TiffErrorCapture capture;
  ScopedTiffOpenOptions opts = MakeTiffOpenOptions(&capture);
  ASSERT_NE(nullptr, opts);
  {
    int fd = open(tif_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    absl::Cleanup close_fd = [fd] { close(fd); };
    ScopedTiff writer(XTIFFFdOpenExt(fd, tif_path.c_str(), "w", opts.get()));
    ASSERT_NE(nullptr, writer);
    std::move(close_fd).Cancel();
    ASSERT_NO_FATAL_FAILURE(WriteAllExtendedGeoTags(writer.get()));
  }
  {
    int fd = open(tif_path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    absl::Cleanup close_fd = [fd] { close(fd); };
    ScopedTiff reader(XTIFFFdOpenExt(fd, tif_path.c_str(), "r", opts.get()));
    ASSERT_NE(nullptr, reader);
    std::move(close_fd).Cancel();
    ASSERT_NO_FATAL_FAILURE(VerifyAllExtendedGeoTags(reader.get()));
    EXPECT_THAT(capture.errors, IsEmpty());
    EXPECT_THAT(capture.warnings, IsEmpty());
  }

  // 3. Invalid 0-byte file descriptors fail cleanly in both XTIFFFdOpen and
  // XTIFFFdOpenExt without leaking descriptors.
  {
    int create_fd = open(empty_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(create_fd, 0);
    ASSERT_EQ(0, close(create_fd));

    int fd1 = open(empty_path.c_str(), O_RDONLY);
    ASSERT_GE(fd1, 0);
    absl::Cleanup close_fd1 = [fd1] { close(fd1); };
    EXPECT_EQ(nullptr, XTIFFFdOpen(fd1, empty_path.c_str(), "r"));

    int fd2 = open(empty_path.c_str(), O_RDONLY);
    ASSERT_GE(fd2, 0);
    absl::Cleanup close_fd2 = [fd2] { close(fd2); };
    EXPECT_EQ(nullptr,
              XTIFFFdOpenExt(fd2, empty_path.c_str(), "r", opts.get()));
    ASSERT_THAT(capture.errors, Not(IsEmpty()));
    EXPECT_THAT(capture.errors[0].module, Not(IsEmpty()));
  }
}

// TODO(schwehr): Calling `_ParentExtender` in `_XTIFFDefaultDirectory`
// (libxtiff/xtiff.c:86) requires installing a process-global
// `TIFFSetTagExtender` hook before the first `XTIFFInitialize()` call in the
// binary, which is omitted to avoid process-global hook coupling across tests.

}  // namespace
}  // namespace libgeotiff
