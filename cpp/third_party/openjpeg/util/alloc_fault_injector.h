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
// Deterministic thread-local allocation failure injector for testing OpenJPEG
// out-of-memory error handling without OS allocation pressure. Must be linked
// via //third_party/openjpeg2:openjpeg2_internal_no_malloc (never alongside
// :openjpeg2_internal or :openjpeg2).

#ifndef THIRD_PARTY_OPENJPEG2_GOOGLE_UTIL_ALLOC_FAULT_INJECTOR_H_
#define THIRD_PARTY_OPENJPEG2_GOOGLE_UTIL_ALLOC_FAULT_INJECTOR_H_

namespace openjpeg2 {

// RAII guard that configures OpenJPEG's opj_*alloc* wrappers on the current
// thread to fail once after `countdown` successful allocations (`countdown ==
// 0` fails the very next allocation; negative values disable fault injection)
// and restores the previous countdown on scope exit.
class ScopedOpjAllocFailCountdown {
 public:
  explicit ScopedOpjAllocFailCountdown(int countdown);
  ~ScopedOpjAllocFailCountdown();

  ScopedOpjAllocFailCountdown(const ScopedOpjAllocFailCountdown&) = delete;
  ScopedOpjAllocFailCountdown& operator=(const ScopedOpjAllocFailCountdown&) =
      delete;

 private:
  int prev_countdown_;
};

// RAII guard enabling zero-initialization in opj_aligned_32_malloc on the
// current thread (no-op under MSan) to prevent upstream unused-SIMD-lane
// signed-overflow UB under ASan malloc_fill_byte.
class ScopedZeroOpjAligned32Alloc {
 public:
  ScopedZeroOpjAligned32Alloc();
  ~ScopedZeroOpjAligned32Alloc();

  ScopedZeroOpjAligned32Alloc(const ScopedZeroOpjAligned32Alloc&) = delete;
  ScopedZeroOpjAligned32Alloc& operator=(const ScopedZeroOpjAligned32Alloc&) =
      delete;

 private:
  bool prev_zero_;
};

}  // namespace openjpeg2

#endif  // THIRD_PARTY_OPENJPEG2_GOOGLE_UTIL_ALLOC_FAULT_INJECTOR_H_
