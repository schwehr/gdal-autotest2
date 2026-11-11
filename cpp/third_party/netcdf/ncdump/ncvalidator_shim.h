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
// One-shot POSIX syscall fault-injection helpers for ncvalidator_test.cc.
// Only active in non-sanitizer builds so ASan/MSan/TSan interceptors remain
// untouched; callers should inspect ncvalidator_shim_is_active().

#ifndef THIRD_PARTY_NETCDF_GOOGLE_NCDUMP_NCVALIDATOR_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_NCDUMP_NCVALIDATOR_SHIM_H_

#ifdef __cplusplus
extern "C" {
#endif

// Returns non-zero if syscall fault injection is active in this build.
int ncvalidator_shim_is_active(void);

// Disarms any pending fault injection.
void ncvalidator_shim_reset(void);

// Arms the next chunk-sized read() on fd >= 3 after skipping
// `skip_chunk_reads` matching calls to fail once with -1 (EIO) and disarm.
void ncvalidator_shim_arm_read_fail(int skip_chunk_reads);

// Arms the next chunk-boundary lseek() on fd >= 3 after skipping
// `skip_chunk_lseeks` matching calls to fail once with -1 (EIO) and disarm.
void ncvalidator_shim_arm_lseek_chunk_fail(int skip_chunk_lseeks);

// Arms the next header-padding repair lseek() on fd >= 3 to fail once with
// -1 (EIO) and disarm.
void ncvalidator_shim_arm_repair_lseek_fail(void);

// Arms the next header-padding repair write() on fd >= 3 to fail once with
// -1 (EIO) and disarm.
void ncvalidator_shim_arm_repair_write_fail(void);

// Arms the next header-padding repair write() on fd >= 3 to perform a short
// write once (fewer than `count` bytes) and disarm.
void ncvalidator_shim_arm_repair_write_short(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_NCDUMP_NCVALIDATOR_SHIM_H_
