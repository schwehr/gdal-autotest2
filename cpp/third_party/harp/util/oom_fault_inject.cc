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

#include "third_party/stcorp_harp/google/util/oom_fault_inject.h"

#include <cerrno>
#include <cstddef>

namespace {

constinit thread_local int g_oom_countdown = 0;
constinit thread_local bool g_oom_triggered = false;
constinit thread_local int g_suppress_oom = 0;

#ifdef __SANITIZE_ALLOC_TOKEN__
#define HARP_NO_ALLOC_TOKEN __attribute__((no_sanitize("alloc-token")))
#else
#define HARP_NO_ALLOC_TOKEN
#endif

HARP_NO_ALLOC_TOKEN bool ShouldFailAllocation() {
  if (g_suppress_oom == 0 && g_oom_countdown > 0 && --g_oom_countdown == 0) {
    g_oom_triggered = true;
    errno = ENOMEM;
    return true;
  }
  return false;
}

}  // namespace

extern "C" {

// Forward declarations matching hashtable.h, udunits2.h, and harp-internal.h.
struct hashtable_struct;
struct harp_dataset_struct;
struct harp_product_metadata_struct;

void* __real_malloc(size_t size);
void* __real_calloc(size_t nmemb, size_t size);
void* __real_realloc(void* ptr, size_t size);
char* __real_strdup(const char* s);
int __real_harp_hashtable_insert_name(struct hashtable_struct* table,
                                      long index, const char* name);
void* __real_ut_read_xml(const char* path);
void* __real_ut_parse(const void* system, const char* str, int encoding);
void* __real_ut_get_converter(void* from, void* to);
int __real_harp_dataset_add_product(
    struct harp_dataset_struct* dataset, const char* source_product,
    struct harp_product_metadata_struct* metadata);

HARP_NO_ALLOC_TOKEN void* __wrap_malloc(size_t size) {
  if (ShouldFailAllocation()) {
    return nullptr;
  }
  return __real_malloc(size);
}

HARP_NO_ALLOC_TOKEN void* __wrap_calloc(size_t nmemb, size_t size) {
  if (ShouldFailAllocation()) {
    return nullptr;
  }
  return __real_calloc(nmemb, size);
}

HARP_NO_ALLOC_TOKEN void* __wrap_realloc(void* ptr, size_t size) {
  if (ShouldFailAllocation()) {
    return nullptr;
  }
  return __real_realloc(ptr, size);
}

HARP_NO_ALLOC_TOKEN char* __wrap_strdup(const char* s) {
  if (ShouldFailAllocation()) {
    return nullptr;
  }
  return __real_strdup(s);
}

// hashtable_insert_name in hashtable.c asserts on malloc failure, so suppress
// OOM injection while inside it.
int __wrap_harp_hashtable_insert_name(struct hashtable_struct* table,
                                      long index, const char* name) {
  stcorp_harp::ScopedSuppressOom suppress;
  return __real_harp_hashtable_insert_name(table, index, name);
}

// udunits2 uses a Flex scanner that calls exit(2) on malloc failure, and
// harp_dataset_add_product leaves a NULL source_product entry on strdup OOM.
void* __wrap_ut_read_xml(const char* path) {
  stcorp_harp::ScopedSuppressOom suppress;
  return __real_ut_read_xml(path);
}

void* __wrap_ut_parse(const void* system, const char* str, int encoding) {
  stcorp_harp::ScopedSuppressOom suppress;
  return __real_ut_parse(system, str, encoding);
}

void* __wrap_ut_get_converter(void* from, void* to) {
  stcorp_harp::ScopedSuppressOom suppress;
  return __real_ut_get_converter(from, to);
}

int __wrap_harp_dataset_add_product(
    struct harp_dataset_struct* dataset, const char* source_product,
    struct harp_product_metadata_struct* metadata) {
  stcorp_harp::ScopedSuppressOom suppress;
  return __real_harp_dataset_add_product(dataset, source_product, metadata);
}

#ifdef __SANITIZE_ALLOC_TOKEN__
void* __real___alloc_token_0_malloc(size_t size);
void* __real___alloc_token_1_malloc(size_t size);
void* __real___alloc_token_9_malloc(size_t size);
void* __real___alloc_token_0_calloc(size_t nmemb, size_t size);
void* __real___alloc_token_1_calloc(size_t nmemb, size_t size);
void* __real___alloc_token_9_calloc(size_t nmemb, size_t size);
void* __real___alloc_token_0_realloc(void* ptr, size_t size);
void* __real___alloc_token_1_realloc(void* ptr, size_t size);
void* __real___alloc_token_9_realloc(void* ptr, size_t size);

HARP_NO_ALLOC_TOKEN void* __wrap___alloc_token_0_malloc(size_t size) {
  return ShouldFailAllocation() ? nullptr : __real___alloc_token_0_malloc(size);
}
HARP_NO_ALLOC_TOKEN void* __wrap___alloc_token_1_malloc(size_t size) {
  return ShouldFailAllocation() ? nullptr : __real___alloc_token_1_malloc(size);
}
HARP_NO_ALLOC_TOKEN void* __wrap___alloc_token_9_malloc(size_t size) {
  return ShouldFailAllocation() ? nullptr : __real___alloc_token_9_malloc(size);
}

HARP_NO_ALLOC_TOKEN void* __wrap___alloc_token_0_calloc(size_t nmemb,
                                                        size_t size) {
  return ShouldFailAllocation() ? nullptr
                                : __real___alloc_token_0_calloc(nmemb, size);
}
HARP_NO_ALLOC_TOKEN void* __wrap___alloc_token_1_calloc(size_t nmemb,
                                                        size_t size) {
  return ShouldFailAllocation() ? nullptr
                                : __real___alloc_token_1_calloc(nmemb, size);
}
HARP_NO_ALLOC_TOKEN void* __wrap___alloc_token_9_calloc(size_t nmemb,
                                                        size_t size) {
  return ShouldFailAllocation() ? nullptr
                                : __real___alloc_token_9_calloc(nmemb, size);
}

HARP_NO_ALLOC_TOKEN void* __wrap___alloc_token_0_realloc(void* ptr,
                                                         size_t size) {
  return ShouldFailAllocation() ? nullptr
                                : __real___alloc_token_0_realloc(ptr, size);
}
HARP_NO_ALLOC_TOKEN void* __wrap___alloc_token_1_realloc(void* ptr,
                                                         size_t size) {
  return ShouldFailAllocation() ? nullptr
                                : __real___alloc_token_1_realloc(ptr, size);
}
HARP_NO_ALLOC_TOKEN void* __wrap___alloc_token_9_realloc(void* ptr,
                                                         size_t size) {
  return ShouldFailAllocation() ? nullptr
                                : __real___alloc_token_9_realloc(ptr, size);
}
#endif  // __SANITIZE_ALLOC_TOKEN__

}  // extern "C"

namespace stcorp_harp {

ScopedOomCountdown::ScopedOomCountdown(int fail_on_nth)
    : prev_countdown_(g_oom_countdown), prev_triggered_(g_oom_triggered) {
  g_oom_countdown = fail_on_nth > 0 ? fail_on_nth : 0;
  g_oom_triggered = false;
}

ScopedOomCountdown::~ScopedOomCountdown() {
  g_oom_countdown = prev_countdown_;
  g_oom_triggered = prev_triggered_;
}

bool ScopedOomCountdown::triggered() const { return g_oom_triggered; }

ScopedSuppressOom::ScopedSuppressOom() { ++g_suppress_oom; }

ScopedSuppressOom::~ScopedSuppressOom() { --g_suppress_oom; }

}  // namespace stcorp_harp
