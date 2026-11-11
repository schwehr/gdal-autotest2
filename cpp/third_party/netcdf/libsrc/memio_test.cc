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

// Unit tests for //third_party/netcdf/libsrc/memio.c.

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
extern "C" {
#include "third_party/netcdf/libsrc/ncio.h"
int memio_create(const char* path, int ioflags, size_t initialsz, off_t igeto,
                 size_t igetsz, size_t* sizehintp, void* parameters,
                 ncio** nciopp, void** const mempp);
int memio_open(const char* path, int ioflags, off_t igeto, size_t igetsz,
               size_t* sizehintp, void* parameters, ncio** nciopp,
               void** const mempp);
int memio_extract(ncio* const nciop, size_t* sizep, void** memoryp);
}  // extern "C"
// clang-format on

namespace netcdf {
namespace {

size_t GetPageSize() {
  const long pg = ::sysconf(_SC_PAGE_SIZE);
  return pg > 0 ? static_cast<size_t>(pg) : 4096U;
}

void CreateClassicSeedImage(int cmode_flags, size_t initialsz,
                            ScopedMemio* out_memio) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("memio_seed.nc", cmode_flags, initialsz, file.ptr()));
  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "x", 4, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(file.get(), "v", NC_INT, 1, &dimid, &varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
  const int values[4] = {10, 20, 30, 40};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(file.get(), varid, values));
  ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), out_memio->get()));
}

TEST(MemioTest, InMemoryCreateRedefNoFillAndOpenMemRoundTrip) {
  const size_t pg = GetPageSize();
  ScopedMemio memio;

  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("memio_rt.nc", NC_CLOBBER, 0, file.ptr()));
    int xdim = -1;
    int vvar = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "x", 3, &xdim));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "v", NC_DOUBLE, 1, &xdim, &vvar));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(file.get(), NC_GLOBAL, "note", 3, "abc"));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));

    const double vals[3] = {1.5, 2.5, 3.5};
    ASSERT_EQ(NC_NOERR, nc_put_var_double(file.get(), vvar, vals));

    // Re-enter define mode, enlarge the header and add an unwritten big
    // variable in NC_NOFILL mode so enddef exercises memio_move and close
    // exercises memio_pad_length past the initial page allocation.
    ASSERT_EQ(NC_NOERR, nc_redef(file.get()));
    int bigdim = -1;
    int bigvar = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "bigdim", pg, &bigdim));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "big", NC_INT, 1, &bigdim, &bigvar));
    const char extra_text[] = "expanded_header_triggers_memio_move";
    ASSERT_EQ(NC_NOERR, nc_put_att_text(file.get(), NC_GLOBAL, "extra",
                                        sizeof(extra_text) - 1, extra_text));
    int old_fill = NC_FILL;
    ASSERT_EQ(NC_NOERR, nc_set_fill(file.get(), NC_NOFILL, &old_fill));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), memio.get()));
    EXPECT_GT(memio.size(), pg);
  }

  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open_mem("memio_rt.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro.ptr()));
  int vvar = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro.get(), "v", &vvar));
  double got[3] = {};
  ASSERT_EQ(NC_NOERR, nc_get_var_double(ro.get(), vvar, got));
  EXPECT_THAT(got, ::testing::ElementsAre(1.5, 2.5, 3.5));
  ASSERT_EQ(NC_NOERR, ro.close());
}

TEST(MemioTest, CreateMemHandlesUnalignedAndAlignedClassicFormats) {
  const size_t pg = GetPageSize();
  ScopedMemio cdf5_img;
  CreateClassicSeedImage(NC_CDF5, 100, &cdf5_img);
  EXPECT_NE(nullptr, cdf5_img.bytes());
  EXPECT_GT(cdf5_img.size(), 0U);

  ScopedMemio offset64_img;
  CreateClassicSeedImage(NC_64BIT_OFFSET, pg, &offset64_img);
  EXPECT_NE(nullptr, offset64_img.bytes());
  EXPECT_GT(offset64_img.size(), 0U);
}

TEST(MemioTest, DisklessWithoutAndWithPersistRoundTrip) {
  {
    TempFile ephemeral("memio_ephemeral.nc");
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create(ephemeral.c_str(), NC_DISKLESS | NC_CLOBBER,
                                  file.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(file.get(), "v", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    const int vals[2] = {11, 22};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(file.get(), varid, vals));
    ASSERT_EQ(NC_NOERR, file.close());
    EXPECT_FALSE(std::filesystem::exists(ephemeral.path()));
  }

  TempFile persisted("memio_persisted.nc");
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create(persisted.c_str(),
                        NC_DISKLESS | NC_PERSIST | NC_CLOBBER, file.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(file.get(), "v", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    const int vals[2] = {100, 200};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(file.get(), varid, vals));
    ASSERT_EQ(NC_NOERR, file.close());
    ASSERT_TRUE(std::filesystem::exists(persisted.path()));
  }

  // Clobber existing writeable persisted file via memio_create persist check.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create(persisted.c_str(),
                        NC_DISKLESS | NC_PERSIST | NC_CLOBBER, file.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(file.get(), "v", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    const int vals[2] = {300, 400};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(file.get(), varid, vals));
    ASSERT_EQ(NC_NOERR, file.close());
  }

  // Reopen with NC_DISKLESS | NC_PERSIST | NC_WRITE, update, and verify.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_open(persisted.c_str(), NC_DISKLESS | NC_PERSIST | NC_WRITE,
                      file.ptr()));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(file.get(), "v", &varid));
    const int vals[2] = {500, 600};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(file.get(), varid, vals));
    ASSERT_EQ(NC_NOERR, file.close());
  }

  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR,
              nc_open(persisted.c_str(), NC_DISKLESS | NC_NOWRITE, ro.ptr()));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro.get(), "v", &varid));
    int got[2] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro.get(), varid, got));
    EXPECT_THAT(got, ::testing::ElementsAre(500, 600));
    ASSERT_EQ(NC_NOERR, ro.close());
  }
}

TEST(MemioTest, OpenMemioOwnershipModesAndLockedLimit) {
  ScopedMemio seed;
  CreateClassicSeedImage(NC_CLOBBER, 0, &seed);
  ASSERT_GT(seed.size(), 0U);

  // 1. Unlocked + NC_NOWRITE: memio_open does not clear info.memory, yet
  // memio_close still owns and frees the buffer on close.
  {
    void* buf = std::malloc(seed.size());
    ASSERT_NE(nullptr, buf);
    std::memcpy(buf, seed.bytes(), seed.size());
    NC_memio info{seed.size(), buf, 0};

    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR,
              nc_open_memio("unlocked_ro.nc", NC_NOWRITE, &info, ro.ptr()));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro.get(), "v", &varid));
    int got[4] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro.get(), varid, got));
    EXPECT_THAT(got, ::testing::ElementsAre(10, 20, 30, 40));
    EXPECT_EQ(NC_EPERM, nc_redef(ro.get()));
    ASSERT_EQ(NC_NOERR, ro.close());
  }

  // 2. Unlocked + NC_WRITE: memio_open takes ownership and clears info.memory.
  {
    void* buf = std::malloc(seed.size());
    ASSERT_NE(nullptr, buf);
    std::memcpy(buf, seed.bytes(), seed.size());
    NC_memio info{seed.size(), buf, 0};

    ScopedNcFile rw;
    ASSERT_EQ(NC_NOERR,
              nc_open_memio("unlocked_rw.nc", NC_WRITE, &info, rw.ptr()));
    EXPECT_EQ(nullptr, info.memory);

    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(rw.get(), "v", &varid));
    const int updated[4] = {101, 202, 303, 404};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(rw.get(), varid, updated));

    ScopedMemio final_memio;
    ASSERT_EQ(NC_NOERR, nc_close_memio(rw.release(), final_memio.get()));

    ScopedNcFile verify;
    ASSERT_EQ(NC_NOERR,
              nc_open_mem("verify_rw.nc", NC_NOWRITE, final_memio.size(),
                          final_memio.get()->memory, verify.ptr()));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(verify.get(), "v", &varid));
    int got[4] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(verify.get(), varid, got));
    EXPECT_THAT(got, ::testing::ElementsAre(101, 202, 303, 404));
    ASSERT_EQ(NC_NOERR, verify.close());
  }

  // 3. Locked + NC_WRITE: in-place writes succeed, but extending past alloc
  // fails cleanly with NC_EINMEMORY.
  {
    std::vector<unsigned char> locked_buf(seed.bytes(),
                                          seed.bytes() + seed.size());
    NC_memio info{locked_buf.size(), locked_buf.data(), NC_MEMIO_LOCKED};
    ScopedNcFile locked_rw;
    ASSERT_EQ(NC_NOERR,
              nc_open_memio("locked_rw.nc", NC_WRITE, &info, locked_rw.ptr()));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(locked_rw.get(), "v", &varid));
    const int updated[4] = {7, 8, 9, 10};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(locked_rw.get(), varid, updated));

    ASSERT_EQ(NC_NOERR, nc_redef(locked_rw.get()));
    int bigdim = -1;
    int bigvar = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_dim(locked_rw.get(), "bigdim", GetPageSize(), &bigdim));
    ASSERT_EQ(NC_NOERR, nc_def_var(locked_rw.get(), "bigvar", NC_DOUBLE, 1,
                                   &bigdim, &bigvar));
    EXPECT_EQ(NC_EINMEMORY, nc_enddef(locked_rw.get()));
    ASSERT_EQ(NC_NOERR, locked_rw.abort());
  }
}

TEST(MemioTest, PersistPermissionsAndMissingFileErrors) {
  {
    TempFile missing("memio_missing.nc");
    ScopedNcFile file;
    EXPECT_EQ(ENOENT, nc_open(missing.c_str(), NC_DISKLESS, file.ptr()));

    size_t sizehint = 0;
    ncio* nciop = nullptr;
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    EXPECT_EQ(ENOENT, memio_open(missing.c_str(), NC_DISKLESS, 0, 0, &sizehint,
                                 nullptr, &nciop, nullptr));
    EXPECT_EQ(nullptr, nciop);
  }

  {
    TempFile missing("memio_missing_inmem_persist.nc");
    std::vector<unsigned char> buf(32, 0);
    NC_memio params{buf.size(), buf.data(), NC_MEMIO_LOCKED};
    size_t sizehint = 0;
    ncio* nciop = nullptr;
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    EXPECT_EQ(ENOENT,
              memio_open(missing.c_str(), NC_INMEMORY | NC_PERSIST | NC_NOWRITE,
                         0, 0, &sizehint, &params, &nciop, nullptr));
    EXPECT_EQ(nullptr, nciop);
  }

  TempFile ro_file("memio_ro_persist.nc");
  {
    ScopedNcFile file;
    // Create via POSIX backend here so MSan is not tripped by memio_create()'s
    // uninitialized 4 KiB page allocation on an empty file; NC_DISKLESS |
    // NC_PERSIST writes are exercised with data earlier in this test.
    ASSERT_EQ(NC_NOERR, nc_create(ro_file.c_str(), NC_CLOBBER, file.ptr()));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    ASSERT_EQ(NC_NOERR, file.close());
  }

  ASSERT_EQ(0, ::chmod(ro_file.c_str(), 0444));
  absl::Cleanup restore_perms = [&] { ::chmod(ro_file.c_str(), 0644); };

  if (::access(ro_file.c_str(), W_OK) != 0) {
    ScopedNcFile fail_create;
    EXPECT_EQ(EPERM,
              nc_create(ro_file.c_str(), NC_DISKLESS | NC_PERSIST | NC_CLOBBER,
                        fail_create.ptr()));

    ScopedNcFile fail_open;
    EXPECT_EQ(EACCES,
              nc_open(ro_file.c_str(), NC_DISKLESS | NC_PERSIST | NC_WRITE,
                      fail_open.ptr()));
  }
}

TEST(MemioTest, DirectMemioAndNcioOperationsCoverAllBranches) {
  const size_t pg = GetPageSize();

  EXPECT_EQ(NC_EINVAL, memio_create(nullptr, NC_INMEMORY, 0, 0, 0, nullptr,
                                    nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_EINVAL, memio_create("", NC_INMEMORY, 0, 0, 0, nullptr, nullptr,
                                    nullptr, nullptr));

  size_t hint = 0;
  EXPECT_EQ(NC_EINVAL, memio_open(nullptr, NC_DISKLESS, 0, 0, &hint, nullptr,
                                  nullptr, nullptr));
  EXPECT_EQ(NC_EINVAL, memio_open("", NC_DISKLESS, 0, 0, &hint, nullptr,
                                  nullptr, nullptr));
  EXPECT_EQ(NC_NOERR, memio_extract(nullptr, nullptr, nullptr));

  // Direct memio_create without igetsz or sizehintp.
  {
    ncio* nciop = nullptr;
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    ASSERT_EQ(NC_NOERR, memio_create("direct_simple.nc", NC_DISKLESS, pg, 0, 0,
                                     nullptr, nullptr, &nciop, nullptr));
    ASSERT_NE(nullptr, nciop);
    EXPECT_EQ(NC_EINVAL, nciop->filesize(nullptr, nullptr));
    EXPECT_EQ(NC_NOERR, nciop->filesize(nciop, nullptr));
    off_t fsz = -1;
    EXPECT_EQ(NC_NOERR, nciop->filesize(nciop, &fsz));
    EXPECT_EQ(static_cast<off_t>(pg), fsz);

    EXPECT_EQ(NC_EINVAL, nciop->pad_length(nullptr, 16));
    EXPECT_EQ(NC_EINVAL, nciop->get(nullptr, 0, 4, 0, nullptr));
    EXPECT_EQ(NC_EINVAL, nciop->move(nullptr, 0, 0, 0, 0));
    EXPECT_EQ(NC_EINVAL, nciop->rel(nullptr, 0, 0));
    EXPECT_EQ(NC_NOERR, nciop->sync(nciop));
    EXPECT_EQ(NC_NOERR, nciop->close(nullptr, 0));

    ncio* to_close = nciop;
    nciop = nullptr;
    EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));
  }

  // Direct memio_create with igetsz != 0, pad_length, move, and memio_extract.
  {
    ncio* nciop = nullptr;
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    void* init_ptr = nullptr;
    size_t sizehint = 0;
    ASSERT_EQ(NC_NOERR, memio_create("direct_ops.nc", NC_INMEMORY, 100, 0, 32,
                                     &sizehint, nullptr, &nciop, &init_ptr));
    ASSERT_NE(nullptr, nciop);
    ASSERT_NE(nullptr, init_ptr);
    EXPECT_EQ(pg, sizehint);
    std::memset(init_ptr, 0, 32);

    // Lock is already held from igetsz=32, so pad_length fails NC_EINMEMORY.
    EXPECT_EQ(NC_EINMEMORY,
              nciop->pad_length(nciop, static_cast<off_t>(2 * pg)));
    ASSERT_EQ(NC_NOERR, nciop->rel(nciop, 0, RGN_MODIFIED));

    // Zero-initialize full initial page so MSan reads inside memmove/pad are
    // clean.
    void* page_ptr = nullptr;
    ASSERT_EQ(NC_NOERR, nciop->get(nciop, 0, pg, RGN_WRITE, &page_ptr));
    std::memset(page_ptr, 0, pg);
    std::memcpy(page_ptr, "ABCDEFGH", 8);
    ASSERT_EQ(NC_NOERR, nciop->rel(nciop, 0, RGN_MODIFIED));

    // Unaligned extend, page-aligned extend, shrink <= alloc, then large jump
    // to exercise realloc moving buffers (memio->modified++).
    ASSERT_EQ(NC_NOERR, nciop->pad_length(nciop, static_cast<off_t>(pg + 50)));
    ASSERT_EQ(NC_NOERR, nciop->pad_length(nciop, static_cast<off_t>(16 * pg)));
    ASSERT_EQ(NC_NOERR, nciop->pad_length(nciop, 128));

    // Exercise memio_move branches:
    // 1) from < to extending past alloc (guarantee extends).
    ASSERT_EQ(NC_NOERR,
              nciop->move(nciop, static_cast<off_t>(18 * pg + 16), 0, 8, 0));
    // 2) from > to overlapping (memmove).
    ASSERT_EQ(NC_NOERR, nciop->move(nciop, 2, 4, 4, 0));
    // 3) to == from && nbytes == 0 -> else { memcpy } non-overlapping branch.
    ASSERT_EQ(NC_NOERR, nciop->move(nciop, 0, 0, 0, 0));

    // get with vpp == nullptr.
    ASSERT_EQ(NC_NOERR, nciop->get(nciop, 0, 8, 0, nullptr));
    ASSERT_EQ(NC_NOERR, nciop->rel(nciop, 0, 0));

    // extract metadata-only, full memory ownership transfer, then empty second
    // extract after memio->memory has been cleared.
    size_t extracted_size = 0;
    EXPECT_EQ(NC_NOERR, memio_extract(nciop, nullptr, nullptr));
    void* extracted_mem = nullptr;
    ASSERT_EQ(NC_NOERR, memio_extract(nciop, &extracted_size, &extracted_mem));
    ScopedCString owned_mem =
        MakeScopedCString(static_cast<char*>(extracted_mem));
    ASSERT_NE(nullptr, owned_mem.get());
    EXPECT_EQ(18 * pg + 24, extracted_size);
    EXPECT_EQ(0, std::memcmp(owned_mem.get() + 18 * pg + 16, "ABCDEFGH", 8));
    void* second_mem = nullptr;
    EXPECT_EQ(NC_NOERR, memio_extract(nciop, &extracted_size, &second_mem));
    EXPECT_EQ(nullptr, second_mem);

    ncio* to_close = nciop;
    nciop = nullptr;
    EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));
  }

  // Direct memio_open edge cases: tiny buffer, page-aligned locked, null
  // nciopp, failed igetsz, and read-only get/move guarantee failures.
  {
    void* small_buf = std::calloc(1, 12);
    ASSERT_NE(nullptr, small_buf);
    NC_memio params{12, small_buf, 0};
    size_t sizehint = 0;
    ncio* nciop = nullptr;
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    ASSERT_EQ(NC_NOERR, memio_open("tiny_unlocked.nc", NC_INMEMORY | NC_WRITE,
                                   0, 0, &sizehint, &params, &nciop, nullptr));
    EXPECT_EQ(nullptr, params.memory);
    EXPECT_GE(sizehint, 8U);
    ncio* to_close = nciop;
    nciop = nullptr;
    EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));
  }

  {
    // Locked tiny buffer (< 16 bytes) clamps alloc back to 12 and sizehint
    // to 8.
    std::vector<unsigned char> tiny(12, 0x5A);
    NC_memio params{tiny.size(), tiny.data(), NC_MEMIO_LOCKED};
    size_t sizehint = 0;
    ncio* nciop = nullptr;
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    ASSERT_EQ(NC_NOERR, memio_open("tiny_locked.nc", NC_INMEMORY | NC_NOWRITE,
                                   0, 0, &sizehint, &params, &nciop, nullptr));
    EXPECT_EQ(8U, sizehint);
    EXPECT_EQ(EPERM, nciop->pad_length(nciop, 32));

    // Trigger guarantee() failure in memio_get and memio_move safely.
    void* bad_ptr = nullptr;
    EXPECT_EQ(EPERM, nciop->get(nciop, 0, 32, 0, &bad_ptr));
    EXPECT_EQ(NC_NOERR, nciop->rel(nciop, 0, 0));
    EXPECT_EQ(EPERM, nciop->move(nciop, 24, 0, 8, 0));

    ncio* to_close = nciop;
    nciop = nullptr;
    EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));
  }

  {
    // Exact page-aligned locked buffer with igetsz != 0, plus nciopp ==
    // nullptr, plus igetsz exceeding alloc causing unwind_open.
    std::vector<unsigned char> page_buf(pg, 0);
    NC_memio params{page_buf.size(), page_buf.data(), NC_MEMIO_LOCKED};
    size_t sizehint = 0;
    ncio* nciop = nullptr;
    absl::Cleanup close_ncio = [&] {
      if (nciop != nullptr) {
        nciop->close(nciop, 0);
      }
    };
    void* ptr = nullptr;
    ASSERT_EQ(NC_NOERR, memio_open("aligned_locked.nc", NC_INMEMORY | NC_WRITE,
                                   0, 8, &sizehint, &params, &nciop, &ptr));
    EXPECT_EQ(page_buf.data(), ptr);
    ASSERT_EQ(NC_NOERR, nciop->rel(nciop, 0, 0));
    ncio* to_close = nciop;
    nciop = nullptr;
    EXPECT_EQ(NC_NOERR, to_close->close(to_close, 0));

    // nciopp == nullptr closes immediately inside memio_open.
    EXPECT_EQ(NC_NOERR, memio_open("null_nciopp.nc", NC_INMEMORY | NC_NOWRITE,
                                   0, 0, &sizehint, &params, nullptr, nullptr));

    // igetsz > alloc on locked buffer triggers unwind_open in memio_open.
    EXPECT_EQ(NC_EINMEMORY,
              memio_open("unwind_igetsz.nc", NC_INMEMORY | NC_WRITE, 0, 2 * pg,
                         &sizehint, &params, &nciop, &ptr));
    EXPECT_EQ(nullptr, nciop);
  }
}

}  // namespace
}  // namespace netcdf
