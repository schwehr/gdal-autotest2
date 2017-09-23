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
//
// Fuzz GeoTIFF format raster grids.
//
// Using the driver Open is not as direct as using GTiffDataset and does not
// allow using all the methods of GTiffDataset, but it's better than GDALOpenEx.
//
// See also:
//   https://trac.osgeo.org/gdal/browser/trunk/gdal/fuzzers/gdal_fuzzer.cpp

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <memory>
#include <string>

#include "logging.h"
#include "third_party/absl/memory/memory.h"
#include "alg/gdal_alg.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_frmts.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_string.h"

typedef std::unique_ptr<char *, std::function<void(char **)>> StringListPtr;

constexpr char kDriverName[] = "GTiff";
constexpr size_t kMaxStrLen = 100000;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const char kFilename[] = "/vsimem/a.tif";
  const string data2(reinterpret_cast<const char *>(data), size);
  autotest2::VsiMemTempWrapper wrapper(kFilename, data2);

  GDALRegister_GTiff();
  GDALDriverManager *drv_manager = GetGDALDriverManager();
  GDALDriver *driver = drv_manager->GetDriverByName(kDriverName);
  CHECK_NOTNULL(driver);

  WithQuietHandler error_handler;
  auto open_info =
      gtl::MakeUnique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);

  auto dataset = absl::WrapUnique(driver->pfnOpen(open_info.get()));

  // If the fuzzer data can't be opened, do not go any further.
  if (dataset == nullptr) return 0;

  // String owned by dataset.
  CHECK_GT(kMaxStrLen, strnlen(dataset->GetProjectionRef(), kMaxStrLen + 1));

  {
    double geotransform[6] = {};
    dataset->GetGeoTransform(geotransform);
  }

  {
    StringListPtr files(dataset->GetFileList(), CSLDestroy);
    CHECK_EQ(CSLCount(files.get()), 1);
  }

  CHECK_LE(0, dataset->GetGCPCount());

  // GCP pointer owned by dataset.
  dataset->GetGCPs();
  // TODO(schwehr): If gcps != nullptr, check more.

  CHECK_GT(kMaxStrLen, strnlen(dataset->GetGCPProjection(), kMaxStrLen + 1));

  const int num_bands = dataset->GetRasterCount();
  if (num_bands == 0) return 0;
  CHECK_GE(num_bands, 0);

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

    // Computing the checksum can take a long time, so do only one band.
    if (band_num > 1) continue;
    GDALChecksumImage(band, 0, 0, xsize, ysize);
  }

  // TODO(schwehr): Enable after fixing EXIFPrintData.
  // CSLDestroy(dataset->GetMetadataDomainList());

  return 0;
}

