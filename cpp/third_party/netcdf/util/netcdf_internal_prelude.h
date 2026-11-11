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
// Safe C++20 prelude for including unguarded internal netCDF headers
// (nc.h, ncdispatch.h, nc3internal.h, nc4internal.h, hdf5internal.h, etc.):
// pre-includes standard C/C++ headers and config.h + netcdf.h at C++ linkage
// before callers open `extern "C" { ... }` around internal C headers.

#ifndef THIRD_PARTY_NETCDF_GOOGLE_UTIL_NETCDF_INTERNAL_PRELUDE_H_
#define THIRD_PARTY_NETCDF_GOOGLE_UTIL_NETCDF_INTERNAL_PRELUDE_H_

// clang-format off
#include <assert.h>    // IWYU pragma: keep
#include <ctype.h>     // IWYU pragma: keep
#include <errno.h>     // IWYU pragma: keep
#include <float.h>     // IWYU pragma: keep
#include <inttypes.h>  // IWYU pragma: keep
#include <limits.h>    // IWYU pragma: keep
#include <math.h>      // IWYU pragma: keep
#include <stdarg.h>    // IWYU pragma: keep
#include <stdbool.h>   // IWYU pragma: keep
#include <stddef.h>    // IWYU pragma: keep
#include <stdint.h>    // IWYU pragma: keep
#include <stdio.h>     // IWYU pragma: keep
#include <stdlib.h>    // IWYU pragma: keep
#include <string.h>    // IWYU pragma: keep
#include <time.h>      // IWYU pragma: keep
#include <unistd.h>    // IWYU pragma: keep

#ifdef __cplusplus
// Google3 `base/types.h` (pulled via Abseil/GUnit) defines
// `using uint64 = uint64_t;` (`unsigned long`), which collides with
// `include/ncconfigure.h` (`typedef unsigned long long uint64;`) when
// `HAVE_UINT64` is unset. Upstream headers never use `uint64`, so suppress the
// redundant typedef in C++ callers while leaving `.c` compilations untouched.
#ifndef HAVE_UINT64
#define HAVE_UINT64 1
#endif
#endif

#include "third_party/netcdf/config.h"              // IWYU pragma: export
#include "third_party/netcdf/include/netcdf.h"      // IWYU pragma: export
#include "third_party/netcdf/include/netcdf_mem.h"  // IWYU pragma: export
#include "third_party/netcdf/include/netcdf_meta.h"  // IWYU pragma: export

#ifdef __cplusplus
// In C, `struct ChunkCache` nested inside `struct NCglobalstate`
// (include/ncglobal.h) has file scope and satisfies `struct NC_VAR_INFO`
// (include/nc4internal.h). In C++, nested structs are scoped to the enclosing
// struct (`NCglobalstate::ChunkCache`), so define layout-identical
// `::ChunkCache` at file scope.
struct ChunkCache {
  size_t size;
  size_t nelems;
  float preemption;
};
#endif

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define typeid nc_typeid_param
#include "third_party/netcdf/include/netcdf_aux.h"     // IWYU pragma: export
#include "third_party/netcdf/include/netcdf_filter.h"  // IWYU pragma: export
#undef typeid
#pragma clang diagnostic pop
// clang-format on

#endif  // THIRD_PARTY_NETCDF_GOOGLE_UTIL_NETCDF_INTERNAL_PRELUDE_H_
