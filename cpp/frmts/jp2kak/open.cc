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

// For debugging issues uncovered by the fuzzer.

#include "base/commandlineflags.h"
#include "base/init_google.h"
#include "base/logging.h"

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <string>

#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/frmts/jp2kak/jp2kakdataset.h"

int main(int argc, char **argv) {
  InitGoogle(argv[0], &argc, &argv, true);

  char *filename = argv[0];
  LOG(INFO) << filename;

  std::unique_ptr<GDALOpenInfo> open_info(
      new GDALOpenInfo(filename, GDAL_OF_READONLY, nullptr));
  if (open_info == nullptr)
      return 0;
  GDALDataset *dataset = JP2KAKDataset::Open(open_info.get());
  if (dataset == nullptr)
      return 0;
  delete dataset;
  return 0;
}
