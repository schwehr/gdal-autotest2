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
// Deterministic thread-local allocation fault injector and ASan SIMD-lane
// zero-initializer replacing src/lib/openjp2/opj_malloc.c for OOM testing.
// Allocation countdowns track direct opj_* allocator calls on the calling
// thread only (excluding worker threads and standard C++ allocations);
// AlignedRealloc consumes a single countdown tick without recursive ticking.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/opj_malloc.c

#include "third_party/openjpeg2/google/util/alloc_fault_injector.h"

#include <stdlib.h>  // For posix_memalign.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace openjpeg2 {
namespace {

thread_local int g_tl_alloc_fail_countdown = -1;
thread_local bool g_tl_zero_aligned_32_alloc = false;

bool ShouldFailOpjAlloc() {
  if (g_tl_alloc_fail_countdown < 0) {
    return false;
  }
  if (g_tl_alloc_fail_countdown == 0) {
    g_tl_alloc_fail_countdown = -1;
    return true;
  }
  --g_tl_alloc_fail_countdown;
  return false;
}

void* AlignedAlloc(size_t alignment, size_t size) {
  if (size == 0U || ShouldFailOpjAlloc()) {
    return nullptr;
  }
  void* ptr = nullptr;
  if (posix_memalign(&ptr, alignment, size) != 0) {
    return nullptr;
  }
  return ptr;
}

void* AlignedRealloc(void* ptr, size_t alignment, size_t size) {
  if (size == 0U || ShouldFailOpjAlloc()) {
    return nullptr;
  }
  void* r_ptr = std::realloc(ptr, size);
  if (r_ptr != nullptr &&
      (reinterpret_cast<uintptr_t>(r_ptr) & (alignment - 1U)) != 0U) {
    void* a_ptr = nullptr;
    if (posix_memalign(&a_ptr, alignment, size) != 0) {
      a_ptr = nullptr;
    } else {
      std::memcpy(a_ptr, r_ptr, size);
    }
    std::free(r_ptr);
    r_ptr = a_ptr;
  }
  return r_ptr;
}

}  // namespace

ScopedOpjAllocFailCountdown::ScopedOpjAllocFailCountdown(int countdown)
    : prev_countdown_(g_tl_alloc_fail_countdown) {
  g_tl_alloc_fail_countdown = countdown;
}

ScopedOpjAllocFailCountdown::~ScopedOpjAllocFailCountdown() {
  g_tl_alloc_fail_countdown = prev_countdown_;
}

ScopedZeroOpjAligned32Alloc::ScopedZeroOpjAligned32Alloc()
    : prev_zero_(g_tl_zero_aligned_32_alloc) {
  g_tl_zero_aligned_32_alloc = true;
}

ScopedZeroOpjAligned32Alloc::~ScopedZeroOpjAligned32Alloc() {
  g_tl_zero_aligned_32_alloc = prev_zero_;
}

}  // namespace openjpeg2

extern "C" {

void* opj_malloc(size_t size);
void* opj_calloc(size_t num, size_t size);
void* opj_aligned_malloc(size_t size);
void* opj_aligned_realloc(void* ptr, size_t size);
void opj_aligned_free(void* ptr);
void* opj_aligned_32_malloc(size_t size);
void* opj_aligned_32_realloc(void* ptr, size_t size);
void* opj_realloc(void* ptr, size_t new_size);
void opj_free(void* ptr);

void* opj_malloc(size_t size) {
  if (size == 0U || openjpeg2::ShouldFailOpjAlloc()) {
    return nullptr;
  }
  return std::malloc(size);
}

void* opj_calloc(size_t num, size_t size) {
  if (num == 0U || size == 0U || openjpeg2::ShouldFailOpjAlloc()) {
    return nullptr;
  }
  return std::calloc(num, size);
}

void* opj_aligned_malloc(size_t size) {
  return openjpeg2::AlignedAlloc(16U, size);
}

void* opj_aligned_realloc(void* ptr, size_t size) {
  return openjpeg2::AlignedRealloc(ptr, 16U, size);
}

void opj_aligned_free(void* ptr) { std::free(ptr); }

void* opj_aligned_32_malloc(size_t size) {
  void* ptr = openjpeg2::AlignedAlloc(32U, size);
#if !defined(MEMORY_SANITIZER)
  if (ptr != nullptr && openjpeg2::g_tl_zero_aligned_32_alloc) {
    std::memset(ptr, 0, size);
  }
#endif
  return ptr;
}

void* opj_aligned_32_realloc(void* ptr, size_t size) {
  return openjpeg2::AlignedRealloc(ptr, 32U, size);
}

void* opj_realloc(void* ptr, size_t new_size) {
  if (new_size == 0U || openjpeg2::ShouldFailOpjAlloc()) {
    return nullptr;
  }
  return std::realloc(ptr, new_size);
}

void opj_free(void* ptr) { std::free(ptr); }

}  // extern "C"
