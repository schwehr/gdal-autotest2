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
#include "base/commandlineflags.h"
#include "logging.h"
#include "third_party/absl/memory/memory.h"
#include "alg/gdal_alg.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal_priv.h"

DEFINE_FLAG(bool, gdal_deep_fuzz, true,
            "Disable to do light fuzzing.  Generally used for faster fuzzing "
            "to generate initial corpus for a fuzzer.");

namespace autotest2 {

typedef std::unique_ptr<char *, std::function<void(char **)>> StringListPtr;

constexpr size_t kMaxStrLen = 100000;

void GDALFuzzOneInput(GDALDataset *dataset) {
  if (dataset == nullptr) return;

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
  bool gdal_deep_fuzz = base::GetFlag(FLAGS_gdal_deep_fuzz);
  if (!gdal_deep_fuzz) return;

  const int num_bands = dataset->GetRasterCount();
  for (int band_num = 1; band_num < num_bands + 1; band_num++) {
    GDALRasterBand *band = dataset->GetRasterBand(band_num);
    CHECK(band);
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
    const int xsize = band->GetXSize();
    const int ysize = band->GetYSize();
    const int64 num_pixels = static_cast<int64>(xsize) * ysize;
    constexpr int kMaxPixels = 1024 * 1024 * 16;
    if (num_pixels > kMaxPixels) {
      // Do not checksum massive rasters.
      continue;
    }

    GDALChecksumImage(band, 0, 0, xsize, ysize);
  }

  // TODO(schwehr): CreateCopy.
}

}  // namespace autotest2
