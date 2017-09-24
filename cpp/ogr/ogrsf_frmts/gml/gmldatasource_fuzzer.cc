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

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <string>

#include "logging.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/fuzzers/ogr.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_feature.h"
#include "ogr/ogrsf_frmts/gml/ogr_gml.h"
#include "ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "port/cpl_error.h"
#include "port/cpl_port.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  WithQuietHandler handler;

  const char kFilename[] = "/vsimem/a.gml";
  const string data2(reinterpret_cast<const char *>(data), size);
  autotest2::VsiMemTempWrapper wrapper(kFilename, data2);
  auto open_info =
      gtl::MakeUnique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);
  std::unique_ptr<OGRGMLDataSource> dataset(new OGRGMLDataSource);
  const int result = dataset->Open(open_info.get());
  CHECK(result == FALSE || result == TRUE);

  autotest2::OGRFuzzOneInput(dataset.get());

  return 0;
}
