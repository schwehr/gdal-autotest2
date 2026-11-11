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

#ifndef THIRD_PARTY_NETCDF_GOOGLE_UTIL_NCGEN_MAIN_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_UTIL_NCGEN_MAIN_SHIM_H_

#include <setjmp.h>

#ifdef __cplusplus
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

extern "C" {
// clang-format off
#include "third_party/netcdf/ncgen/includes.h"  // IWYU pragma: export
#include "third_party/netcdf/ncgen/odom.h"      // IWYU pragma: export
#include "third_party/netcdf/ncgen/dump.h"      // IWYU pragma: export
// clang-format on

// `ncgen/util.c` defines `freeSymbol()`, while `ncgen/genlib.h` has an
// unimplemented lowercase typo (`freesymbol()`).
void freeSymbol(Symbol* sym);
#endif

// Arms a sigjmp_buf to catch calls to `finalize_netcdf(retcode)` from fatal
// `ncgen` helpers (such as `semerror()`) in-process without terminating the
// test binary. Pass NULL to disarm and revert to calling `exit(retcode)`.
void ncgen_shim_set_finalize_jmp(sigjmp_buf* env, int* out_retcode);
void ncgen_shim_reset_globals(void);

#ifdef __cplusplus
}
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_UTIL_NCGEN_MAIN_SHIM_H_
