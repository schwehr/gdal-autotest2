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

#include "third_party/netcdf/google/ncdump/ncvalidator_shim.h"

#undef _FORTIFY_SOURCE
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__has_feature)
#define NCVALIDATOR_SHIM_HAS_FEATURE(x) __has_feature(x)
#else
#define NCVALIDATOR_SHIM_HAS_FEATURE(x) 0
#endif

#if !defined(ADDRESS_SANITIZER) && !defined(MEMORY_SANITIZER) &&      \
    !defined(THREAD_SANITIZER) && !defined(HWADDRESS_SANITIZER) &&   \
    !defined(LEAK_SANITIZER) && !defined(DATAFLOW_SANITIZER) &&      \
    !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && \
    !NCVALIDATOR_SHIM_HAS_FEATURE(address_sanitizer) &&              \
    !NCVALIDATOR_SHIM_HAS_FEATURE(memory_sanitizer) &&               \
    !NCVALIDATOR_SHIM_HAS_FEATURE(thread_sanitizer) &&               \
    !NCVALIDATOR_SHIM_HAS_FEATURE(hwaddress_sanitizer) &&            \
    !NCVALIDATOR_SHIM_HAS_FEATURE(leak_sanitizer) &&                 \
    !NCVALIDATOR_SHIM_HAS_FEATURE(dataflow_sanitizer)
#define NCVALIDATOR_SHIM_ACTIVE 1
#else
#define NCVALIDATOR_SHIM_ACTIVE 0
#endif

#if NCVALIDATOR_SHIM_ACTIVE

enum {
  kShimFaultNone = 0,
  kShimFaultReadChunkFail = 1,
  kShimFaultLseekChunkFail = 2,
  kShimFaultLseekRepairFail = 3,
  kShimFaultWriteRepairFail = 4,
  kShimFaultWriteRepairShort = 5,
};

// Matches NC_DEFAULT_CHUNKSIZE in ncvalidator.c.
static const size_t kShimChunkSize = 1048576U;

static int g_fault_mode = kShimFaultNone;
static int g_skip_hits = 0;

int ncvalidator_shim_is_active(void) { return 1; }

void ncvalidator_shim_reset(void) {
  g_fault_mode = kShimFaultNone;
  g_skip_hits = 0;
}

void ncvalidator_shim_arm_read_fail(int skip_chunk_reads) {
  g_fault_mode = kShimFaultReadChunkFail;
  g_skip_hits = skip_chunk_reads;
}

void ncvalidator_shim_arm_lseek_chunk_fail(int skip_chunk_lseeks) {
  g_fault_mode = kShimFaultLseekChunkFail;
  g_skip_hits = skip_chunk_lseeks;
}

void ncvalidator_shim_arm_repair_lseek_fail(void) {
  g_fault_mode = kShimFaultLseekRepairFail;
  g_skip_hits = 0;
}

void ncvalidator_shim_arm_repair_write_fail(void) {
  g_fault_mode = kShimFaultWriteRepairFail;
  g_skip_hits = 0;
}

void ncvalidator_shim_arm_repair_write_short(void) {
  g_fault_mode = kShimFaultWriteRepairShort;
  g_skip_hits = 0;
}

static ssize_t shim_do_read(int fd, void* buf, size_t count) {
  if (g_fault_mode == kShimFaultReadChunkFail && fd >= 3 &&
      count == kShimChunkSize) {
    if (g_skip_hits > 0) {
      g_skip_hits--;
    } else {
      g_fault_mode = kShimFaultNone;
      errno = EIO;
      return -1;
    }
  }
  return (ssize_t)syscall(SYS_read, fd, buf, count);
}

ssize_t read(int fd, void* buf, size_t count) {
  return shim_do_read(fd, buf, count);
}

extern ssize_t __read_chk(int fd, void* buf, size_t count, size_t buflen);

ssize_t __read_chk(int fd, void* buf, size_t count, size_t buflen) {
  if (count > buflen) {
    abort();
  }
  return shim_do_read(fd, buf, count);
}

ssize_t write(int fd, const void* buf, size_t count) {
  if (fd >= 3 && count >= 1U && count <= 3U) {
    if (g_fault_mode == kShimFaultWriteRepairFail) {
      g_fault_mode = kShimFaultNone;
      errno = EIO;
      return -1;
    }
    if (g_fault_mode == kShimFaultWriteRepairShort) {
      g_fault_mode = kShimFaultNone;
      if (count > 1U) {
        return (ssize_t)syscall(SYS_write, fd, buf, count - 1U);
      }
      return 0;
    }
  }
  return (ssize_t)syscall(SYS_write, fd, buf, count);
}

off_t lseek(int fd, off_t offset, int whence) {
  if (fd >= 3 && whence == SEEK_SET) {
    if (g_fault_mode == kShimFaultLseekChunkFail &&
        offset >= (off_t)(kShimChunkSize - 1024U)) {
      if (g_skip_hits > 0) {
        g_skip_hits--;
      } else {
        g_fault_mode = kShimFaultNone;
        errno = EIO;
        return -1;
      }
    } else if (g_fault_mode == kShimFaultLseekRepairFail && offset > 0 &&
               offset < 4096) {
      g_fault_mode = kShimFaultNone;
      errno = EIO;
      return -1;
    }
  }
  return (off_t)syscall(SYS_lseek, fd, (long)offset, whence);
}

#else  // !NCVALIDATOR_SHIM_ACTIVE

int ncvalidator_shim_is_active(void) { return 0; }
void ncvalidator_shim_reset(void) {}
void ncvalidator_shim_arm_read_fail(int skip_chunk_reads) {
  (void)skip_chunk_reads;
}
void ncvalidator_shim_arm_lseek_chunk_fail(int skip_chunk_lseeks) {
  (void)skip_chunk_lseeks;
}
void ncvalidator_shim_arm_repair_lseek_fail(void) {}
void ncvalidator_shim_arm_repair_write_fail(void) {}
void ncvalidator_shim_arm_repair_write_short(void) {}

#endif  // NCVALIDATOR_SHIM_ACTIVE
