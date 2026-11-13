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
//
// Safe C++20 prelude for including internal libharp/*.h headers, which lack
// `extern "C"` guards and depend on standard C/POSIX headers and coda.h.

#ifndef THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_HARP_INTERNAL_PRELUDE_H_
#define THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_HARP_INTERNAL_PRELUDE_H_

// clang-format off
#include "third_party/stcorp_harp/libharp/config.h"  // IWYU pragma: export
#include "third_party/stcorp_harp/libharp/harp.h"    // IWYU pragma: export
#include "third_party/stcorp_coda/libcoda/coda.h"    // IWYU pragma: export

// Pre-include standard C/POSIX headers at C++ linkage before entering extern "C".
#include <assert.h>     // IWYU pragma: keep
#include <ctype.h>      // IWYU pragma: keep
#include <dirent.h>     // IWYU pragma: keep
#include <errno.h>      // IWYU pragma: keep
#include <fcntl.h>      // IWYU pragma: keep
#include <float.h>      // IWYU pragma: keep
#include <inttypes.h>   // IWYU pragma: keep
#include <limits.h>     // IWYU pragma: keep
#include <locale.h>     // IWYU pragma: keep
#include <math.h>       // IWYU pragma: keep
#include <stdarg.h>     // IWYU pragma: keep
#include <stdbool.h>    // IWYU pragma: keep
#include <stddef.h>     // IWYU pragma: keep
#include <stdint.h>     // IWYU pragma: keep
#include <stdio.h>      // IWYU pragma: keep
#include <stdlib.h>     // IWYU pragma: keep
#include <string.h>     // IWYU pragma: keep
#include <strings.h>    // IWYU pragma: keep
#include <sys/stat.h>   // IWYU pragma: keep
#include <sys/types.h>  // IWYU pragma: keep
#include <unistd.h>     // IWYU pragma: keep
// clang-format on

extern "C" {
#include "third_party/stcorp_harp/libharp/harp-area-mask.h"  // IWYU pragma: export
#include "third_party/stcorp_harp/libharp/harp-chemistry.h"  // IWYU pragma: export
#include "third_party/stcorp_harp/libharp/harp-constants.h"  // IWYU pragma: export
#include "third_party/stcorp_harp/libharp/harp-csv.h"  // IWYU pragma: export
#include "third_party/stcorp_harp/libharp/harp-dimension-mask.h"  // IWYU pragma: export
#include "third_party/stcorp_harp/libharp/harp-filter-collocation.h"  // IWYU pragma: export
#include "third_party/stcorp_harp/libharp/harp-filter.h"  // IWYU pragma: export
#include "third_party/stcorp_harp/libharp/harp-geometry.h"  // IWYU pragma: export
#include "third_party/stcorp_harp/libharp/harp-ingestion.h"  // IWYU pragma: export
#include "third_party/stcorp_harp/libharp/harp-internal.h"  // IWYU pragma: export
#include "third_party/stcorp_harp/libharp/harp-operation.h"  // IWYU pragma: export
#include "third_party/stcorp_harp/libharp/harp-program.h"  // IWYU pragma: export
#include "third_party/stcorp_harp/libharp/harp-vertical-profiles.h"  // IWYU pragma: export
}  // extern "C"

static_assert(sizeof(long) == 8,
              "libharp config.h hardcodes SIZEOF_LONG 8 (LP64 required)");

#endif  // THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_HARP_INTERNAL_PRELUDE_H_
