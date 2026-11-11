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

#include "third_party/netcdf/google/libdispatch/ds3util_shim.h"

#include <stddef.h>

#include "third_party/netcdf/config.h"
#include "third_party/netcdf/include/ncglobal.h"

void ds3util_shim_clear_aws(struct NCglobalstate* gs) {
  if (gs == NULL) {
    return;
  }
  NC_clearawsparams(&gs->aws);
}
