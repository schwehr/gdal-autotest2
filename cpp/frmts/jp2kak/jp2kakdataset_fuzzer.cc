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

#include "frmts/jp2kak/jp2kakdataset.h"

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <string>

#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const char kFilename[] = "/vsimem/a.jp2";
  const string data2(reinterpret_cast<const char *>(data), size);
  autotest2::VsiMemTempWrapper wrapper(kFilename, data2);
  std::unique_ptr<GDALOpenInfo> open_info(
      new GDALOpenInfo(kFilename, GDAL_OF_READONLY, nullptr));
  if (open_info == nullptr)
      return 0;
  int result = JP2KAKDataset::Identify(open_info.get());
  CHECK_LE(-1, result);
  CHECK_GE(1, result);
  GDALDataset *dataset = JP2KAKDataset::Open(open_info.get());
  if (dataset == nullptr)
      return 0;
  delete dataset;
  return 0;
}
