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
// Safe C++20 prelude for including CODA internal headers (coda-internal.h and
// friends):
//   1. Pre-includes standard C/POSIX and GoogleTest headers at C++ linkage so
//      system/libc++ templates are never instantiated inside extern "C".
//   2. Includes config.h with -Wmacro-redefined suppressed for _XOPEN_SOURCE.
//   3. Includes coda-internal.h inside extern "C" before coda.h is ever seen,
//      preserving CODA_INTERNAL symbol visibility and C linkage.

#ifndef THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_CODA_INTERNAL_PRELUDE_H_
#define THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_CODA_INTERNAL_PRELUDE_H_

// clang-format off
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <memory>

#include "testing/base/public/gmock.h"  // IWYU pragma: export
#include "testing/base/public/gunit.h"  // IWYU pragma: export

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include "third_party/stcorp_coda/libcoda/config.h"  // IWYU pragma: export
#pragma clang diagnostic pop

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-internal.h"  // IWYU pragma: export

struct coda_detection_rule_struct;

// Headerless non-static functions defined in libcoda/*.c.
int coda_product_check(coda_product* product, int full_read_check,
                       void (*callbackfunc)(coda_cursor*, const char*, void*),
                       void* userdata);
void coda_detection_tree_delete(void* detection_tree);
int coda_detection_tree_add_rule(
    void* detection_tree, struct coda_detection_rule_struct* detection_rule);
}  // extern "C"
// clang-format on

namespace stcorp_coda {

struct DynamicTypeDeleter {
  template <typename T>
  void operator()(T* ptr) const {
    if (ptr != nullptr) {
      coda_dynamic_type_delete(reinterpret_cast<coda_dynamic_type*>(ptr));
    }
  }
};

template <typename T = coda_dynamic_type>
using UniqueDynamicType = std::unique_ptr<T, DynamicTypeDeleter>;

}  // namespace stcorp_coda

#endif  // THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_CODA_INTERNAL_PRELUDE_H_
