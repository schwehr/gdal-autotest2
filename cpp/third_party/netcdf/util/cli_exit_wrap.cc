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

// Linker `--wrap=exit` hook for CLI unit tests running C `main()` functions
// inside `RunCliInForkedChild()`. In a forked child, C `exit()` flushes stdio,
// dumps coverage counters, and `_exit()`s directly without running parent
// `atexit` handlers or LSan exit checks on intentionally unfreed CLI globals.

#include <signal.h>
#include <unistd.h>

#include <cstdio>

extern "C" {
void __gcov_dump() __attribute__((weak));
[[noreturn]] void __real_exit(int status);
extern volatile sig_atomic_t netcdf_google_in_forked_child;

[[noreturn]] void __wrap_exit(int status) {
  if (netcdf_google_in_forked_child) {
    std::fflush(nullptr);
    if (&__gcov_dump != nullptr) {
      __gcov_dump();
    }
    ::_exit(status);
  }
  __real_exit(status);
}
}  // extern "C"
