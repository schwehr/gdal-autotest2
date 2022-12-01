// Copyright 2020 Google Inc. All Rights Reserved.
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

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>

#include "logging.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/fuzzers/gdal.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_frmts.h"
#include "gcore/gdal_priv.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const std::string data2(reinterpret_cast<const char *>(data), size);

  WithQuietHandler error_handler;
  GDALRegister_SRTMHGT();

  // Filenames taken from special cases in the driver and the checks for
  // north/south and east/west
  for (const auto &filename :
       {"/vsimem/n00e006.raw",
        "/vsimem/a.hgt",
        "/vsimem/n00e006.hgt",
        "/vsimem/n00w006.hgt",
        "/vsimem/s00e006.hgt",
        "/vsimem/s00w006.hgt",
        "/vsimem/n00e006.hgt.gz"
        "/vsimem/n00e006.hgt.zip"
        "/vsimem/n00e006.srtmswbd.raw.zip"}) {
    autotest2::VsiMemTempWrapper wrapper(filename, data2);
    auto dataset = GDALOpen(filename, GA_ReadOnly);
    if (dataset == nullptr) continue;
    autotest2::GDALFuzzOneInput(static_cast<GDALDataset *>(dataset));
    GDALClose(dataset);
  }
  return 0;
}
