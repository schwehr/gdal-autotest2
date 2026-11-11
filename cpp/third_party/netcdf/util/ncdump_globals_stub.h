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

#ifndef THIRD_PARTY_NETCDF_GOOGLE_UTIL_NCDUMP_GLOBALS_STUB_H_
#define THIRD_PARTY_NETCDF_GOOGLE_UTIL_NCDUMP_GLOBALS_STUB_H_

#ifdef __cplusplus
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

extern "C" {
// clang-format off
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define false false
#define true true
#define this nc_this_param
#define class nc_class_field
#define typeid nc_typeid_param

#include "third_party/netcdf/include/ncbytes.h"    // IWYU pragma: export
#include "third_party/netcdf/ncdump/utils.h"       // IWYU pragma: export
#include "third_party/netcdf/ncdump/nccomps.h"     // IWYU pragma: export
#include "third_party/netcdf/ncdump/nctime0.h"     // IWYU pragma: export
#include "third_party/netcdf/ncdump/dumplib.h"     // IWYU pragma: export
#include "third_party/netcdf/ncdump/ncdump.h"      // IWYU pragma: export
#include "third_party/netcdf/ncdump/vardata.h"     // IWYU pragma: export
#include "third_party/netcdf/ncdump/indent.h"      // IWYU pragma: export
#include "third_party/netcdf/ncdump/chunkspec.h"   // IWYU pragma: export
#include "third_party/netcdf/ncdump/dimmap.h"      // IWYU pragma: export
#include "third_party/netcdf/ncdump/list.h"        // IWYU pragma: export
#include "third_party/netcdf/ncdump/nciter.h"      // IWYU pragma: export

#undef typeid
#undef class
#undef this
#undef true
#undef false
#pragma clang diagnostic pop
// clang-format on

int NC4print(NCbytes* buf, int ncid);
extern char* progname;
extern fspec_t formatting_specs;
#endif

void ncdump_stub_reset_globals(void);

#ifdef __cplusplus
}
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_UTIL_NCDUMP_GLOBALS_STUB_H_
