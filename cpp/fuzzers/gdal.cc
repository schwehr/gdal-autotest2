// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Raster driver fuzzer.

#include "gcore/gdal.h"
#include "commandlineflags.h"
#include "logging.h"
#include "third_party/absl/flags/flag.h"
#include "third_party/absl/memory/memory.h"
#include "alg/gdal_alg.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal_priv.h"

// Parsing the command line requires that the calling target is built with this
// build dependency.  It is okay to go without it.
//   //security/fuzzing/blaze:default_init_google_for_cc_fuzz_target
ABSL_FLAG(bool, gdal_deep_fuzz, true,
          "Disable to do light fuzzing.  Generally used for faster fuzzing "
          "to generate initial corpus for a fuzzer.");

namespace autotest2 {

typedef std::unique_ptr<char *, std::function<void(char **)>> StringListPtr;

void GDALFuzzOneInput(GDALDataset *dataset) {
  constexpr size_t kMaxStrLen = 100000;
  // How many pixels are we willing to checksum?
  constexpr int kMaxPixels = 1024 * 1024 * 16;

  if (dataset == nullptr) return;

  // Limit to 1 GB
  CPLSetConfigOption("GDAL_CACHEMAX", "1000");

  // String owned by dataset.
  CHECK_GT(kMaxStrLen, strnlen(dataset->GetProjectionRef(), kMaxStrLen + 1));

  double geotransform[6] = {};
  dataset->GetGeoTransform(geotransform);

  StringListPtr files(dataset->GetFileList(), CSLDestroy);
  CHECK_GE(CSLCount(files.get()), 1);
  CHECK_LE(CSLCount(files.get()), 10);

  CHECK_LE(0, dataset->GetGCPCount());

  // GCP pointer owned by dataset.
  dataset->GetGCPs();
  // TODO(schwehr): If gcps != nullptr, check more.

  CHECK_GT(kMaxStrLen, strnlen(dataset->GetGCPProjection(), kMaxStrLen + 1));

  CSLDestroy(dataset->GetMetadataDomainList());

  // If doing a light fuzzing, call it quits now.
  bool gdal_deep_fuzz = absl::GetFlag(FLAGS_gdal_deep_fuzz);
  if (!gdal_deep_fuzz) return;

  const int num_bands = dataset->GetRasterCount();
  for (int band_num = 1; band_num < num_bands + 1; band_num++) {
    GDALRasterBand *band = dataset->GetRasterBand(band_num);
    CHECK(band);

    // First check overviews.
    const int num_overviews = band->GetOverviewCount();
    for (int overview_num = 0; overview_num < num_overviews; overview_num++) {
      GDALRasterBand *overview = band->GetOverview(overview_num);
      if (overview == nullptr) {
        // Not in the API description, but it happens with at least HFA.
        continue;
      }
      int block_xsize = 0;
      int block_ysize = 0;
      band->GetBlockSize(&block_xsize, &block_ysize);
      GDALDataType type = band->GetRasterDataType();
      CHECK_GE(static_cast<int>(type), 0);
      CHECK_LE(static_cast<int>(type), GDT_TypeCount);
      if (block_xsize == 0 || block_ysize == 0 ||
          block_xsize > std::numeric_limits<int>::max() / block_ysize) {
        // Not safe to do anything more with this band.
        continue;
      }
      const int xsize = overview->GetXSize();
      const int ysize = overview->GetYSize();
      const int64 num_pixels = static_cast<int64>(xsize) * ysize;
      if (num_pixels > kMaxPixels) {
        // Do not checksum massive rasters.
        continue;
      }
#if !defined(MEMORY_SANITIZER)
      GDALChecksumImage(overview, 0, 0, xsize, ysize);
#endif
    }

    // Check all the bands.
    int block_xsize = 0;
    int block_ysize = 0;
    band->GetBlockSize(&block_xsize, &block_ysize);
    GDALDataType type = band->GetRasterDataType();
    CHECK_GE(static_cast<int>(type), 0);
    CHECK_LE(static_cast<int>(type), GDT_TypeCount);
    // Don't allow overflow:
    //   block_xsize * block_ysize is not greater than ::max().
    if (block_xsize == 0 || block_ysize == 0 ||
        block_xsize > std::numeric_limits<int>::max() / block_ysize) {
      // Not safe to do anything more with this band.
      continue;
    }
    const int xsize = band->GetXSize();
    const int ysize = band->GetYSize();
    const int64 num_pixels = static_cast<int64>(xsize) * ysize;
    if (num_pixels > kMaxPixels) {
      // Do not checksum massive rasters.
      continue;
    }

#if !defined(MEMORY_SANITIZER)
    // TODO(schwehr): Drivers such as grib and hfa fail MSAN.
    GDALChecksumImage(band, 0, 0, xsize, ysize);
#endif
  }

  // TODO(schwehr): CreateCopy.
}

}  // namespace autotest2
