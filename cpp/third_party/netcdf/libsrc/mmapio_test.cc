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

// Unit tests for //third_party/netcdf/libsrc/mmapio.c.
//
// libsrc/ncio.c does not compile in USE_MMAP selection in this configuration,
// so tests invoke mmapio_create() and mmapio_open() directly.

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fstream>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
extern "C" {
#include "third_party/netcdf/libsrc/ncio.h"
int mmapio_create(const char* path, int ioflags, size_t initialsz, off_t igeto,
                  size_t igetsz, size_t* sizehintp, void* parameters,
                  ncio** nciopp, void** const mempp);
int mmapio_open(const char* path, int ioflags, off_t igeto, size_t igetsz,
                size_t* sizehintp, void* parameters, ncio** nciopp,
                void** const mempp);
}  // extern "C"
// clang-format on

namespace netcdf {
namespace {

size_t GetPageSize() {
  const long pg = ::sysconf(_SC_PAGE_SIZE);
  return pg > 0 ? static_cast<size_t>(pg) : 4096U;
}

TEST(MmapioTest, CreateValidatesArgumentsAndAnonymousMappings) {
  const size_t pg = GetPageSize();

  EXPECT_EQ(NC_EINVAL, mmapio_create(nullptr, NC_DISKLESS, 0, 0, 0, nullptr,
                                     nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_EINVAL, mmapio_create("", NC_DISKLESS, 0, 0, 0, nullptr, nullptr,
                                     nullptr, nullptr));
  EXPECT_EQ(NC_EDISKLESS, mmapio_create("nc4_invalid.nc", NC_NETCDF4, 0, 0, 0,
                                        nullptr, nullptr, nullptr, nullptr));

  // Anonymous (!persist) with initialsz=0, sizehintp=nullptr, igetsz=0.
  {
    ncio* nciop = nullptr;
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    ASSERT_EQ(NC_NOERR, mmapio_create("anon_zero.nc", NC_DISKLESS, 0, 0, 0,
                                      nullptr, nullptr, &nciop, nullptr));
    ASSERT_NE(nullptr, nciop);
    ncio* to_close = nciop;
    nciop = nullptr;
    EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));
  }

  // Anonymous (!persist) with unaligned initialsz, sizehintp, and igetsz != 0.
  {
    ncio* nciop = nullptr;
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    size_t sizehint = 0;
    void* mem = nullptr;
    ASSERT_EQ(NC_NOERR, mmapio_create("anon_unaligned.nc", NC_DISKLESS, 123, 0,
                                      32, &sizehint, nullptr, &nciop, &mem));
    ASSERT_NE(nullptr, nciop);
    ASSERT_NE(nullptr, mem);
    EXPECT_EQ(pg, sizehint);
    std::memcpy(mem, "ANONYMOUS_MMAP_TEST", 19);
    ASSERT_EQ(NC_NOERR, nciop->rel(nciop, 0, RGN_MODIFIED));

    ncio* to_close = nciop;
    nciop = nullptr;
    EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));
  }

  // Anonymous (!persist) with page-aligned initialsz.
  {
    ncio* nciop = nullptr;
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    size_t sizehint = 0;
    ASSERT_EQ(NC_NOERR, mmapio_create("anon_aligned.nc", NC_DISKLESS, pg, 0, 0,
                                      &sizehint, nullptr, &nciop, nullptr));
    EXPECT_EQ(pg, sizehint);
    ncio* to_close = nciop;
    nciop = nullptr;
    EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));
  }
}

TEST(MmapioTest, PersistCreateOpenModifyAndReadOnlyRoundTrip) {
  const size_t pg = GetPageSize();
  TempFile file("mmapio_persist.dat");

  {
    ncio* nciop = nullptr;
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    size_t sizehint = 0;
    void* ptr = nullptr;
    ASSERT_EQ(NC_NOERR, mmapio_create(file.c_str(), NC_PERSIST | NC_CLOBBER, 0,
                                      0, 16, &sizehint, nullptr, &nciop, &ptr));
    ASSERT_NE(nullptr, nciop);
    ASSERT_NE(nullptr, ptr);
    std::memcpy(ptr, "MMAPIO_DATA_1234", 16);
    ASSERT_EQ(NC_NOERR, nciop->rel(nciop, 0, RGN_MODIFIED));
    ASSERT_EQ(NC_NOERR, nciop->sync(nciop));

    off_t fsz = -1;
    ASSERT_EQ(NC_NOERR, nciop->filesize(nciop, &fsz));
    EXPECT_EQ(16, fsz);

    ncio* to_close = nciop;
    nciop = nullptr;
    EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));
  }

  // Reopen read-write via mmapio_open with igetsz != 0 and update prefix.
  {
    ncio* nciop = nullptr;
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    size_t sizehint = 0;
    void* ptr = nullptr;
    ASSERT_EQ(NC_NOERR, mmapio_open(file.c_str(), NC_WRITE, 0, 16, &sizehint,
                                    nullptr, &nciop, &ptr));
    ASSERT_NE(nullptr, nciop);
    ASSERT_NE(nullptr, ptr);
    EXPECT_EQ(pg / 2U, sizehint);
    EXPECT_EQ(0, std::memcmp(ptr, "MMAPIO_DATA_1234", 16));
    std::memcpy(ptr, "TEST", 4);
    ASSERT_EQ(NC_NOERR, nciop->rel(nciop, 0, RGN_MODIFIED));

    ncio* to_close = nciop;
    nciop = nullptr;
    EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));
  }

  // Reopen read-only and verify both persisted data and read-only failures.
  {
    ncio* nciop = nullptr;
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    size_t sizehint = 0;
    ASSERT_EQ(NC_NOERR, mmapio_open(file.c_str(), NC_NOWRITE, 0, 0, &sizehint,
                                    nullptr, &nciop, nullptr));
    void* ptr = nullptr;
    ASSERT_EQ(NC_NOERR, nciop->get(nciop, 0, 16, 0, &ptr));
    EXPECT_EQ(0, std::memcmp(ptr, "TESTIO_DATA_1234", 16));
    ASSERT_EQ(NC_NOERR, nciop->rel(nciop, 0, 0));

    EXPECT_EQ(EPERM, nciop->pad_length(nciop, static_cast<off_t>(pg)));
    EXPECT_EQ(EPERM, nciop->get(nciop, 0, pg + 16, 0, &ptr));
    EXPECT_EQ(NC_NOERR, nciop->rel(nciop, 0, 0));

    ncio* to_close = nciop;
    nciop = nullptr;
    EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));
  }
}

TEST(MmapioTest, OpenHandlesErrorsSmallFilesAndSizehints) {
  size_t sizehint = 0;
  ncio* nciop = nullptr;
  EXPECT_EQ(EINVAL, mmapio_open(nullptr, NC_NOWRITE, 0, 0, &sizehint, nullptr,
                                &nciop, nullptr));
  EXPECT_EQ(EINVAL, mmapio_open("", NC_NOWRITE, 0, 0, &sizehint, nullptr,
                                &nciop, nullptr));

  TempFile missing("mmapio_missing.dat");
  EXPECT_EQ(ENOENT, mmapio_open(missing.c_str(), NC_NOWRITE, 0, 0, &sizehint,
                                nullptr, &nciop, nullptr));
  EXPECT_EQ(nullptr, nciop);

  TempFile small_file("mmapio_small.dat");
  {
    std::ofstream out(small_file.path(), std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out.write("0123456789", 10);
    ASSERT_TRUE(out.good());
  }

  // Open with sizehint > filesize (exercises filesize = sizehint and >= 8).
  {
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    sizehint = 1024;
    ASSERT_EQ(NC_NOERR, mmapio_open(small_file.c_str(), NC_NOWRITE, 0, 4,
                                    &sizehint, nullptr, &nciop, nullptr));
    EXPECT_EQ(512U, sizehint);
    off_t fsz = -1;
    ASSERT_EQ(NC_NOERR, nciop->filesize(nciop, &fsz));
    EXPECT_EQ(1024, fsz);
    ASSERT_EQ(NC_NOERR, nciop->rel(nciop, 0, 0));

    ncio* to_close = nciop;
    nciop = nullptr;
    EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));
  }

  // Open with tiny sizehint < filesize so half filesize rounds down to < 8 and
  // clamps to 8.
  {
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    sizehint = 4;
    ASSERT_EQ(NC_NOERR, mmapio_open(small_file.c_str(), NC_NOWRITE, 0, 0,
                                    &sizehint, nullptr, &nciop, nullptr));
    EXPECT_EQ(8U, sizehint);
    ncio* to_close = nciop;
    nciop = nullptr;
    EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));
  }

  ASSERT_EQ(0, ::chmod(small_file.c_str(), 0444));
  absl::Cleanup restore_perms = [&] { ::chmod(small_file.c_str(), 0644); };
  if (::access(small_file.c_str(), W_OK) != 0) {
    EXPECT_EQ(EACCES, mmapio_open(small_file.c_str(), NC_WRITE, 0, 0, &sizehint,
                                  nullptr, &nciop, nullptr));
    EXPECT_EQ(nullptr, nciop);
  }
}

TEST(MmapioTest, OperationsHandleNullsLockingInBoundsPadAndMoves) {
  const size_t pg = GetPageSize();
  ncio* nciop = nullptr;
  absl::Cleanup close_ncio = [&] {
    if (nciop != nullptr) {
      nciop->close(nciop, 0);
    }
  };
  size_t sizehint = 0;
  ASSERT_EQ(NC_NOERR, mmapio_create("mmapio_ops.nc", NC_DISKLESS, pg, 0, 0,
                                    &sizehint, nullptr, &nciop, nullptr));
  ASSERT_NE(nullptr, nciop);

  EXPECT_EQ(NC_EINVAL, nciop->filesize(nullptr, nullptr));
  EXPECT_EQ(NC_NOERR, nciop->filesize(nciop, nullptr));
  EXPECT_EQ(NC_EINVAL, nciop->pad_length(nullptr, 16));
  EXPECT_EQ(NC_EINVAL, nciop->get(nullptr, 0, 4, 0, nullptr));
  EXPECT_EQ(NC_EINVAL, nciop->move(nullptr, 0, 0, 0, 0));
  EXPECT_EQ(NC_EINVAL, nciop->rel(nullptr, 0, 0));
  EXPECT_EQ(NC_NOERR, nciop->close(nullptr, 0));

  // Hold a locked region: pad_length and guarantee via move fail NC_EDISKLESS.
  void* ptr = nullptr;
  ASSERT_EQ(NC_NOERR, nciop->get(nciop, 0, 32, RGN_WRITE, &ptr));
  std::memcpy(ptr, "0123456789ABCDEF", 16);
  EXPECT_EQ(NC_EDISKLESS, nciop->pad_length(nciop, static_cast<off_t>(2 * pg)));
  EXPECT_EQ(NC_EDISKLESS,
            nciop->move(nciop, static_cast<off_t>(pg + 32), 0, 16, 0));
  ASSERT_EQ(NC_NOERR, nciop->rel(nciop, 0, RGN_MODIFIED));

  // In-bounds pad_length updates logical size without extending mapping.
  ASSERT_EQ(NC_NOERR, nciop->pad_length(nciop, 256));
  off_t fsz = -1;
  ASSERT_EQ(NC_NOERR, nciop->filesize(nciop, &fsz));
  EXPECT_EQ(256, fsz);

  // Move from < to within alloc, overlapping from > to, and zero-byte
  // non-overlap.
  ASSERT_EQ(NC_NOERR, nciop->move(nciop, 64, 0, 16, 0));
  ASSERT_EQ(NC_NOERR, nciop->move(nciop, 2, 4, 8, 0));
  ASSERT_EQ(NC_NOERR, nciop->move(nciop, 0, 0, 0, 0));

  // get with vpp == nullptr succeeds.
  ASSERT_EQ(NC_NOERR, nciop->get(nciop, 64, 16, 0, nullptr));
  ASSERT_EQ(NC_NOERR, nciop->rel(nciop, 64, 0));

  void* moved = nullptr;
  ASSERT_EQ(NC_NOERR, nciop->get(nciop, 64, 16, 0, &moved));
  EXPECT_EQ(0, std::memcmp(moved, "0123456789ABCDEF", 16));
  ASSERT_EQ(NC_NOERR, nciop->rel(nciop, 64, 0));

  ncio* to_close = nciop;
  nciop = nullptr;
  EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));
}

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer) || __has_feature(hwaddress_sanitizer)
#define NETCDF_TEST_HAS_SANITIZER 1
#endif
#endif
#if defined(ADDRESS_SANITIZER) || defined(MEMORY_SANITIZER) ||   \
    defined(THREAD_SANITIZER) || defined(HWADDRESS_SANITIZER) || \
    defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define NETCDF_TEST_HAS_SANITIZER 1
#endif

#ifndef NETCDF_TEST_HAS_SANITIZER
TEST(MmapioTest, PersistNoclobberErrorAndMappingExtension) {
  // Upstream mmapio_create passes NULL to mmapio_close -> munmap(NULL, alloc)
  // on open error, which triggers sanitizer shadow faults in munmap hooks.
  {
    TempFile existing("mmapio_existing.dat");
    {
      std::ofstream out(existing.path(), std::ios::binary);
      ASSERT_TRUE(out.is_open());
      out.write("x", 1);
    }
    ncio* nciop = nullptr;
    EXPECT_EQ(EEXIST,
              mmapio_create(existing.c_str(), NC_PERSIST | NC_NOCLOBBER, 0, 0,
                            0, nullptr, nullptr, &nciop, nullptr));
    EXPECT_EQ(nullptr, nciop);
  }

  // Upstream mmapio_pad_length passes mmapio->alloc as byte count when calling
  // write(mapfd, "", mmapio->alloc), which over-reads "" under sanitizers.
  {
    const size_t pg = GetPageSize();
    ncio* nciop = nullptr;
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    size_t sizehint = 0;
    void* ptr = nullptr;
    ASSERT_EQ(NC_NOERR, mmapio_create("anon_extend.nc", NC_DISKLESS, pg, 0, 16,
                                      &sizehint, nullptr, &nciop, &ptr));
    std::memcpy(ptr, "EXTEND78", 8);
    ASSERT_EQ(NC_NOERR, nciop->rel(nciop, 0, RGN_MODIFIED));

    ASSERT_EQ(NC_NOERR, nciop->pad_length(nciop, static_cast<off_t>(pg + 100)));
    ASSERT_EQ(NC_NOERR, nciop->pad_length(nciop, static_cast<off_t>(3 * pg)));
    ASSERT_EQ(NC_NOERR,
              nciop->move(nciop, static_cast<off_t>(3 * pg + 32), 0, 8, 0));

    void* moved = nullptr;
    ASSERT_EQ(NC_NOERR,
              nciop->get(nciop, static_cast<off_t>(3 * pg + 32), 8, 0, &moved));
    EXPECT_EQ(0, std::memcmp(moved, "EXTEND78", 8));
    ASSERT_EQ(NC_NOERR, nciop->rel(nciop, static_cast<off_t>(3 * pg + 32), 0));

    ncio* to_close = nciop;
    nciop = nullptr;
    EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));
  }
}
#endif  // !NETCDF_TEST_HAS_SANITIZER

}  // namespace
}  // namespace netcdf
