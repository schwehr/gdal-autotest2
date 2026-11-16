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
// C++-safe wrapper for OpenJPEG internal headers. Include C++ standard library,
// GoogleTest, and Abseil headers before this header if needed; all C system
// headers pulled in by opj_includes.h are pre-included outside extern "C", and
// OPJ_SKIP_POISON is defined to prevent #pragma GCC poison in opj_malloc.h.

#ifndef THIRD_PARTY_OPENJPEG2_GOOGLE_UTIL_OPJ_INTERNAL_H_
#define THIRD_PARTY_OPENJPEG2_GOOGLE_UTIL_OPJ_INTERNAL_H_

// Pre-include system C headers outside extern "C" so libc++ C wrappers are not
// first entered inside an extern "C" linkage block.
#include <assert.h>
#include <ctype.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <memory.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

#include "third_party/openjpeg2/src/lib/openjp2/openjpeg.h"

#ifndef OPJ_SKIP_POISON
#define OPJ_SKIP_POISON
#endif

extern "C" {
#include "third_party/openjpeg2/src/lib/openjp2/opj_includes.h"

// Defined in src/lib/openjp2/ht_dec.c and forward-declared in t1.c without a
// public or internal header declaration. Because C linkage provides no
// cross-TU signature checking, keep this declaration synced with ht_dec.c on
// upstream OpenJPEG upgrades.
OPJ_BOOL opj_t1_ht_decode_cblk(opj_t1_t* t1, opj_tcd_cblk_dec_t* cblk,
                               OPJ_UINT32 orient, OPJ_UINT32 roishift,
                               OPJ_UINT32 cblksty, opj_event_mgr_t* p_manager,
                               opj_mutex_t* p_manager_mutex,
                               OPJ_BOOL check_pterm);
}  // extern "C"

#endif  // THIRD_PARTY_OPENJPEG2_GOOGLE_UTIL_OPJ_INTERNAL_H_
