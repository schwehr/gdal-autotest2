// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/netcdf/google/util/ncdump_globals_stub.h"

#include <stddef.h>

// clang-format off
#include "third_party/netcdf/config.h"
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/ncdump/utils.h"
#include "third_party/netcdf/ncdump/nccomps.h"
#include "third_party/netcdf/ncdump/ncdump.h"
// clang-format on

char* progname = "ncdump_test";
fspec_t formatting_specs = {0};

void ncdump_stub_reset_globals(void) {
  static const fspec_t kDefaultSpecs = {0};
  progname = "ncdump_test";
  formatting_specs = kDefaultSpecs;
}
