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
// Fuzz AAIGrid and GRASS grid format raster grids.
//
// TODO(schwehr): Create something that can be reused for all raster fuzzers.
// TODO(schwehr): Try with and without PAM and an optional xml sidecar.
// TODO(schwehr): Switch this fuzzer to proto fuzzer to allow for a PRJ file.
//
// See also:
//   https://trac.osgeo.org/gdal/browser/trunk/gdal/fuzzers/gdal_fuzzer.cpp

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <string>

#include "logging.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/fuzzers/gdal.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "frmts/aaigrid/aaigriddataset.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_string.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const char kFilename[] = "/vsimem/a.asc";
  const string data2(reinterpret_cast<const char *>(data), size);
  autotest2::VsiMemTempWrapper wrapper(kFilename, data2);

  WithQuietHandler error_handler;
  auto open_info =
      gtl::MakeUnique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);
  int result = AAIGDataset::Identify(open_info.get());
  CHECK_LE(-1, result);
  CHECK_GE(1, result);
  auto dataset = absl::WrapUnique(AAIGDataset::Open(open_info.get()));

  // If the fuzzer data can't be opened, do not go any further.
  if (dataset == nullptr) return 0;

  autotest2::GDALFuzzOneInput(dataset.get());

  return 0;
}
