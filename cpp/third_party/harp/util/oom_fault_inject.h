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
// Thread-local linker-wrapped malloc/calloc/realloc/strdup fault injection for
// libharp OOM error-path unit testing.
//
// Note: OOM injection is permanently suppressed inside five upstream symbols
// (see oom_fault_inject.cc and util/BUILD):
//   - harp_hashtable_insert_name (hashtable.c asserts on malloc failure)
//   - ut_read_xml / ut_parse / ut_get_converter (udunits2 Flex calls exit(2))
//   - harp_dataset_add_product (leaves NULL source_product entry on strdup OOM)

#ifndef THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_OOM_FAULT_INJECT_H_
#define THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_OOM_FAULT_INJECT_H_

#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {

// RAII scope that injects a single OOM failure on the `fail_on_nth`-th
// non-suppressed malloc/calloc/realloc/strdup call on the current thread.
class ScopedOomCountdown {
 public:
  explicit ScopedOomCountdown(int fail_on_nth);
  ~ScopedOomCountdown();

  ScopedOomCountdown(const ScopedOomCountdown&) = delete;
  ScopedOomCountdown& operator=(const ScopedOomCountdown&) = delete;

  // Returns true if the countdown reached zero and injected nullptr.
  bool triggered() const;

 private:
  int prev_countdown_ = 0;
  bool prev_triggered_ = false;
};

// RAII scope that suppresses OOM countdown decrementing/triggering during
// test setup or cleanup steps inside a sweep iteration.
class ScopedSuppressOom {
 public:
  ScopedSuppressOom();
  ~ScopedSuppressOom();

  ScopedSuppressOom(const ScopedSuppressOom&) = delete;
  ScopedSuppressOom& operator=(const ScopedSuppressOom&) = delete;
};

// Repeatedly invokes `fn()` while arming ScopedOomCountdown(1, 2, ..., max)
// until `fn()` completes without triggering the countdown. Returns the number
// of allocations swept (0 if `fn()` performed no non-suppressed allocations).
template <typename Fn>
int SweepOomUntilSuccess(Fn&& fn, int max_allocs = 4096) {
  int swept = 0;
  for (int n = 1; n <= max_allocs; ++n) {
    harp_set_error(HARP_SUCCESS, nullptr);
    ScopedOomCountdown guard(n);
    fn();
    if (!guard.triggered()) {
      return swept;
    }
    swept = n;
  }
  ADD_FAILURE() << "SweepOomUntilSuccess reached max_allocs=" << max_allocs
                << " without completing an allocation-free pass";
  return swept;
}

}  // namespace stcorp_harp

#endif  // THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_OOM_FAULT_INJECT_H_
