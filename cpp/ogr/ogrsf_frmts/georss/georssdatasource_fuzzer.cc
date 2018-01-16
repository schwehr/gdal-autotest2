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

#include "base/logging.h"
#include "absl/memory/memory.h"
#include "autotest2/cpp/fuzzers/ogr.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogrsf_frmts/georss/ogr_georss.h"
#include "port/cpl_error.h"
#include "port/cpl_port.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  WithQuietHandler handler;

  const char kFilename[] = "/vsimem/a.xml";
  const string data2(reinterpret_cast<const char *>(data), size);
  autotest2::VsiMemTempWrapper file(kFilename, data2);
  auto dataset = absl::make_unique<OGRGeoRSSDataSource>();
  const int result = dataset->Open(kFilename, FALSE);
  CHECK(result == FALSE || result == TRUE);

  if (!result) return 0;

  autotest2::OGRFuzzOneInput(dataset.get());

  return 0;
}
