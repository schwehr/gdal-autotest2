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

#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/fuzzers/gdal.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "frmts/raw/envidataset.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_port.h"

// TODO(schwehr): Make this a proto fuzzer.

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 2) return 0;
  WithQuietHandler handler;

  const char kFilenameHdr[] = "/vsimem/a.hdr";
  const char kFilenameDat[] = "/vsimem/a.dat";
  const size_t half = size / 2;
  const std::string data_hdr(reinterpret_cast<const char *>(data), half);
  const std::string data_dat(reinterpret_cast<const char *>(data + half),
                             size - half);
  autotest2::VsiMemTempWrapper hdr(kFilenameHdr, data_hdr);
  autotest2::VsiMemTempWrapper dat(kFilenameDat, data_dat);
  auto open_info =
      absl::make_unique<GDALOpenInfo>(kFilenameDat, GDAL_OF_READONLY, nullptr);
  auto dataset = absl::WrapUnique(ENVIDataset::Open(open_info.get()));

  if (dataset == nullptr) return 0;

  autotest2::GDALFuzzOneInput(dataset.get());

  return 0;
}
