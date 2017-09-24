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
#include <string>

#include "third_party/absl/strings/internal/str_split_internal.h"
#include "third_party/absl/strings/str_split.h"
#include "autotest2/cpp/util/cpl_cstringlist.h"
#include "autotest2/cpp/util/error_handler.h"
#include "ogr/ogr_spatialref.h"
#include "port/cpl_conv.h"
#include "port/cpl_port.h"
#include "port/cpl_string.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const string data2(reinterpret_cast<const char *>(data), size);

  WithQuietHandler error_handler;

  OGRSpatialReference srs;
  srs.importFromProj4(data2.c_str());
  srs.Validate();
  char *wkt = nullptr;
  srs.exportToWkt(&wkt);
  CPLFree(wkt);
  srs.morphFromESRI();
  srs.Validate();
  srs.morphToESRI();
  srs.Validate();
  srs.Fixup();
  srs.Clear();
  return 0;
}
