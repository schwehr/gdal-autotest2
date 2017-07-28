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

// cpl_port.h must be first.
#include "port/cpl_port.h"

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <string>

#include "logging.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogrsf_frmts/csv/ogr_csv.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const char kFilename[] = "/vsimem/a";
  const string data2(reinterpret_cast<const char *>(data), size);
  autotest2::VsiMemTempWrapper wrapper(kFilename, data2);
  std::unique_ptr<OGRCSVDataSource> dataset(new OGRCSVDataSource);
  const int result = dataset->Open(kFilename, FALSE, FALSE, nullptr);
  CHECK(result == FALSE || result == TRUE);
  return 0;
}
