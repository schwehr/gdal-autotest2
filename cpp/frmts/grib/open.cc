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

#include <memory>

#include "base/init_google.h"
#include "logging.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/util/vsimem.h"
#include "frmts/grib/gribdataset.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"

int main(int argc, char **argv) {
  InitGoogle(argv[0], &argc, &argv, true);

  char *filename = argv[0];
  LOG(INFO) << filename;

  auto open_info =
      gtl::MakeUnique<GDALOpenInfo>(filename, GDAL_OF_READONLY, nullptr);
  auto dataset = gtl::WrapUnique(GRIBDataset::Open(open_info.get()));
  return 0;
}
