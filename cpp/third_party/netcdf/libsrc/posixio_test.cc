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

// Unit tests for //third_party/netcdf/libsrc/posixio.c.
//
// Exercises POSIX buffered I/O (`ncio_px_*`) and `NC_SHARE` unbuffered I/O
// (`ncio_spx_*`) through both public NetCDF-3 disk APIs and direct
// `posixio_create` / `posixio_open` / `ncio_*` dispatch calls under
// `::testing::TempDir()`.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libsrc/posixio.c

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
#include "third_party/netcdf/libsrc/ncio.h"

int posixio_create(const char* path, int ioflags, size_t initialsz, off_t igeto,
                   size_t igetsz, size_t* sizehintp, void* parameters,
                   ncio** nciopp, void** const igetvpp);
int posixio_open(const char* path, int ioflags, off_t igeto, size_t igetsz,
                 size_t* sizehintp, void* parameters, ncio** nciopp,
                 void** const igetvpp);
}  // extern "C"

namespace netcdf {
namespace {

constexpr size_t kMinBlockSize = 256;
constexpr size_t kMaxBlockSize = 268435456;

class ScopedNcio {
 public:
  ScopedNcio() = default;
  explicit ScopedNcio(ncio* nciop) : nciop_(nciop) {}

  ~ScopedNcio() { reset(); }

  ScopedNcio(const ScopedNcio&) = delete;
  ScopedNcio& operator=(const ScopedNcio&) = delete;

  int close(int do_unlink = 0) {
    if (nciop_ != nullptr) {
      ncio* p = std::exchange(nciop_, nullptr);
      return ncio_close(p, do_unlink);
    }
    return NC_NOERR;
  }

  void reset(ncio* next = nullptr) {
    if (nciop_ != nullptr) {
      ncio* p = std::exchange(nciop_, nullptr);
      static_cast<void>(ncio_close(p, 0));
    }
    nciop_ = next;
  }

  ncio* get() const { return nciop_; }
  ncio** ptr() {
    reset();
    return &nciop_;
  }

 private:
  ncio* nciop_ = nullptr;
};

class ScopedFd {
 public:
  ScopedFd() = default;
  explicit ScopedFd(int fd) : fd_(fd) {}

  ~ScopedFd() { reset(); }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  void reset(int next = -1) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = next;
  }

  int get() const { return fd_; }

 private:
  int fd_ = -1;
};

std::vector<unsigned char> MakePattern(size_t size, unsigned char seed) {
  std::vector<unsigned char> out(size);
  for (size_t i = 0; i < size; ++i) {
    out[i] = static_cast<unsigned char>((i * 131U + seed) & 0xFFU);
  }
  return out;
}

TEST(PosixioTest, CreateAndOpenRejectInvalidPathsAndRespectNoclobber) {
  size_t hint = kMinBlockSize;
  ScopedNcio ncio;

  EXPECT_EQ(EINVAL, posixio_create(nullptr, NC_CLOBBER, 0, 0, 0, &hint, nullptr,
                                   ncio.ptr(), nullptr));
  EXPECT_EQ(EINVAL, posixio_create("", NC_CLOBBER, 0, 0, 0, &hint, nullptr,
                                   ncio.ptr(), nullptr));
  EXPECT_EQ(EINVAL, posixio_open(nullptr, NC_NOWRITE, 0, 0, &hint, nullptr,
                                 ncio.ptr(), nullptr));
  EXPECT_EQ(EINVAL, posixio_open("", NC_NOWRITE, 0, 0, &hint, nullptr,
                                 ncio.ptr(), nullptr));

  TempDir missing_root("posixio_missing_parent");
  const std::filesystem::path missing_in_dir =
      missing_root.child("absent_subdir/create_fail.nc");
  EXPECT_EQ(ENOENT, posixio_create(missing_in_dir.c_str(), NC_CLOBBER, 0, 0, 0,
                                   &hint, nullptr, ncio.ptr(), nullptr));

  TempFile absent("posixio_open_missing.nc");
  EXPECT_EQ(ENOENT, posixio_open(absent.c_str(), NC_NOWRITE, 0, 0, &hint,
                                 nullptr, ncio.ptr(), nullptr));

  TempFile existing("posixio_clobber.nc");
  hint = kMinBlockSize;
  ASSERT_EQ(NC_NOERR, posixio_create(existing.c_str(), NC_CLOBBER, 64, 0, 0,
                                     &hint, nullptr, ncio.ptr(), nullptr));
  ASSERT_EQ(NC_NOERR, ncio.close(0));

  EXPECT_EQ(EEXIST, posixio_create(existing.c_str(), NC_NOCLOBBER, 64, 0, 0,
                                   &hint, nullptr, ncio.ptr(), nullptr));
  EXPECT_TRUE(std::filesystem::exists(existing.path()));

  hint = kMinBlockSize;
  ASSERT_EQ(NC_NOERR, posixio_create(existing.c_str(), NC_CLOBBER, 0, 0, 0,
                                     &hint, nullptr, ncio.ptr(), nullptr));
  off_t truncated_size = -1;
  ASSERT_EQ(NC_NOERR, ncio_filesize(ncio.get(), &truncated_size));
  EXPECT_EQ(0, truncated_size);
  EXPECT_EQ(NC_NOERR, ncio.close(0));
}

TEST(PosixioTest, CreateAndOpenClampAndRoundSizeHints) {
  TempFile file("posixio_sizehints.nc");
  ScopedNcio ncio;

  size_t hint = NC_SIZEHINT_DEFAULT;
  ASSERT_EQ(NC_NOERR, posixio_create(file.c_str(), NC_CLOBBER, 0, 0, 0, &hint,
                                     nullptr, ncio.ptr(), nullptr));
  EXPECT_GE(hint, size_t{8192});
  ASSERT_EQ(NC_NOERR, ncio.close(0));

  hint = 100;
  ASSERT_EQ(NC_NOERR, posixio_create(file.c_str(), NC_CLOBBER, 0, 0, 0, &hint,
                                     nullptr, ncio.ptr(), nullptr));
  EXPECT_GE(hint, size_t{8192});
  ASSERT_EQ(NC_NOERR, ncio.close(0));

  hint = 259;
  ASSERT_EQ(NC_NOERR, posixio_create(file.c_str(), NC_CLOBBER, 0, 0, 0, &hint,
                                     nullptr, ncio.ptr(), nullptr));
  EXPECT_EQ(size_t{264}, hint);
  ASSERT_EQ(NC_NOERR, ncio.close(0));

  // NC_SHARE avoids zero-filling 2*NCIO_MAXBLOCKSIZE during create while still
  // testing the upper clamp branch.
  hint = kMaxBlockSize + 1024;
  ASSERT_EQ(NC_NOERR, posixio_create(file.c_str(), NC_CLOBBER | NC_SHARE, 0, 0,
                                     0, &hint, nullptr, ncio.ptr(), nullptr));
  EXPECT_EQ(kMaxBlockSize, hint);
  ASSERT_EQ(NC_NOERR, ncio.close(0));

  hint = 64;
  ASSERT_EQ(NC_NOERR, posixio_open(file.c_str(), NC_NOWRITE, 0, 0, &hint,
                                   nullptr, ncio.ptr(), nullptr));
  EXPECT_GE(hint, size_t{8192});
  ASSERT_EQ(NC_NOERR, ncio.close(0));

  hint = 257;
  ASSERT_EQ(NC_NOERR, posixio_open(file.c_str(), NC_NOWRITE, 0, 0, &hint,
                                   nullptr, ncio.ptr(), nullptr));
  EXPECT_EQ(size_t{264}, hint);
  ASSERT_EQ(NC_NOERR, ncio.close(0));

  hint = kMaxBlockSize + 4096;
  ASSERT_EQ(NC_NOERR, posixio_open(file.c_str(), NC_NOWRITE | NC_SHARE, 0, 0,
                                   &hint, nullptr, ncio.ptr(), nullptr));
  EXPECT_EQ(kMaxBlockSize, hint);
  EXPECT_EQ(NC_NOERR, ncio.close(0));
}

TEST(PosixioTest, CreateAndOpenHandleInitialSizeInitialGetAndUnwinds) {
  TempFile file("posixio_init_get.nc");
  ScopedNcio ncio;

  const std::vector<unsigned char> header = MakePattern(32, 11);
  size_t hint = kMinBlockSize;
  void* vp = nullptr;
  ASSERT_EQ(NC_NOERR, posixio_create(file.c_str(), NC_CLOBBER, 0, 0, 32, &hint,
                                     nullptr, ncio.ptr(), &vp));
  ASSERT_NE(nullptr, vp);
  std::memcpy(vp, header.data(), header.size());
  ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, RGN_MODIFIED));
  off_t size_after_create = -1;
  ASSERT_EQ(NC_NOERR, ncio_filesize(ncio.get(), &size_after_create));
  EXPECT_GE(size_after_create, 32);
  ASSERT_EQ(NC_NOERR, ncio.close(0));

  vp = nullptr;
  hint = kMinBlockSize;
  ASSERT_EQ(NC_NOERR, posixio_open(file.c_str(), NC_NOWRITE, 0, 16, &hint,
                                   nullptr, ncio.ptr(), &vp));
  ASSERT_NE(nullptr, vp);
  const auto* bytes = static_cast<const unsigned char*>(vp);
  EXPECT_EQ(0, std::memcmp(bytes, header.data(), 16));
  ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, 0));
  ASSERT_EQ(NC_NOERR, ncio.close(0));

  hint = kMinBlockSize;
  ASSERT_EQ(NC_NOERR, posixio_create(file.c_str(), NC_CLOBBER, 1024, 0, 0,
                                     &hint, nullptr, ncio.ptr(), nullptr));
  off_t grown_size = -1;
  ASSERT_EQ(NC_NOERR, ncio_filesize(ncio.get(), &grown_size));
  EXPECT_EQ(1024, grown_size);
  ASSERT_EQ(NC_NOERR, ncio.close(0));

  // initialsz smaller than sizeof(long) causes fgrow's negative lseek to fail
  // and exercises posixio_create's unwind_open path (which unlinks the file).
  hint = kMinBlockSize;
  EXPECT_EQ(EINVAL, posixio_create(file.c_str(), NC_CLOBBER, 1, 0, 0, &hint,
                                   nullptr, ncio.ptr(), nullptr));
  EXPECT_FALSE(std::filesystem::exists(file.path()));

  // igetsz > 2 * blksz fails nciop->get() with E2BIG and unlinks via unwind.
  hint = kMinBlockSize;
  EXPECT_EQ(E2BIG, posixio_create(file.c_str(), NC_CLOBBER, 0, 0, 600, &hint,
                                  nullptr, ncio.ptr(), &vp));
  EXPECT_FALSE(std::filesystem::exists(file.path()));

  // Recreate a 600-byte file, then trigger igetsz > 2 * blksz unwind on open.
  hint = kMinBlockSize;
  ASSERT_EQ(NC_NOERR, posixio_create(file.c_str(), NC_CLOBBER, 600, 0, 0, &hint,
                                     nullptr, ncio.ptr(), nullptr));
  ASSERT_EQ(NC_NOERR, ncio.close(0));

  hint = kMinBlockSize;
  EXPECT_EQ(E2BIG, posixio_open(file.c_str(), NC_NOWRITE, 0, 600, &hint,
                                nullptr, ncio.ptr(), &vp));
  EXPECT_TRUE(std::filesystem::exists(file.path()));
}

TEST(PosixioTest, PxGetValidationAndReadOnlyPermissionChecks) {
  TempFile file("posixio_ro_checks.nc");
  ScopedNcio ncio;

  size_t hint = kMinBlockSize;
  ASSERT_EQ(NC_NOERR, posixio_create(file.c_str(), NC_CLOBBER, 512, 0, 0, &hint,
                                     nullptr, ncio.ptr(), nullptr));
  ASSERT_EQ(NC_NOERR, ncio.close(0));

  hint = kMinBlockSize;
  ASSERT_EQ(NC_NOERR, posixio_open(file.c_str(), NC_NOWRITE, 0, 0, &hint,
                                   nullptr, ncio.ptr(), nullptr));
  ncio_pad_lengthfunc* const pad_fn = ncio.get()->pad_length;
  ncio_closefunc* const close_fn = ncio.get()->close;

  void* vp = nullptr;
  EXPECT_EQ(EPERM, ncio_get(ncio.get(), 0, 16, RGN_WRITE, &vp));
  EXPECT_EQ(EPERM, ncio_move(ncio.get(), 16, 0, 8, RGN_WRITE));
  EXPECT_EQ(EPERM, ncio_pad_length(ncio.get(), 1024));

  ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 0, 16, 0, &vp));
  EXPECT_EQ(EPERM, ncio_rel(ncio.get(), 0, RGN_MODIFIED));
  ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, 0));

  EXPECT_EQ(NC_ENOTNC, ncio_get(ncio.get(), 0, 0, 0, &vp));
  EXPECT_EQ(NC_ENOTNC,
            ncio_get(ncio.get(), 0, static_cast<size_t>(INT32_MAX), 0, &vp));
  EXPECT_EQ(NC_ENOTNC, ncio_get(ncio.get(), -1, 4, 0, &vp));
  EXPECT_EQ(E2BIG, ncio_get(ncio.get(), 0, 513, 0, &vp));
  EXPECT_EQ(E2BIG, ncio_get(ncio.get(), 200, 350, 0, &vp));

  ASSERT_EQ(NC_NOERR, ncio.close(0));
  EXPECT_EQ(EINVAL, pad_fn(nullptr, 0));
  EXPECT_EQ(EINVAL, close_fn(nullptr, 0));
}

TEST(PosixioTest, PxGetCoversNofillUpperHalfPageBelowAndShortReads) {
  // Exercise the `bf_cnt <= blksz` upper-half branch ("nofill bug" fix) both
  // without and with RGN_MODIFIED set.
  {
    TempFile nofill_file("posixio_nofill_branch.nc");
    ScopedNcio ncio;
    size_t hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR, posixio_create(nofill_file.c_str(), NC_CLOBBER, 2048, 0,
                                       0, &hint, nullptr, ncio.ptr(), nullptr));
    void* vp = nullptr;
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 384, 200, RGN_WRITE, &vp));
    const std::vector<unsigned char> seg1 = MakePattern(200, 21);
    std::memcpy(vp, seg1.data(), seg1.size());
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 384, RGN_MODIFIED));
    ASSERT_EQ(NC_NOERR, ncio.close(0));

    hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR, posixio_open(nofill_file.c_str(), NC_NOWRITE, 0, 0,
                                     &hint, nullptr, ncio.ptr(), nullptr));
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 384, 200, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, seg1.data(), seg1.size()));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 384, 0));
    ASSERT_EQ(NC_NOERR, ncio.close(0));
  }

  {
    TempFile nofill_mod_file("posixio_nofill_mod_branch.nc");
    ScopedNcio ncio;
    size_t hint = kMinBlockSize;
    void* vp = nullptr;
    ASSERT_EQ(NC_NOERR,
              posixio_create(nofill_mod_file.c_str(), NC_CLOBBER, 2048, 0, 32,
                             &hint, nullptr, ncio.ptr(), &vp));
    const std::vector<unsigned char> hdr = MakePattern(32, 31);
    std::memcpy(vp, hdr.data(), hdr.size());
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, RGN_MODIFIED));

    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 384, 200, RGN_WRITE, &vp));
    const std::vector<unsigned char> seg2 = MakePattern(200, 41);
    std::memcpy(vp, seg2.data(), seg2.size());
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 384, RGN_MODIFIED));
    ASSERT_EQ(NC_NOERR, ncio.close(0));

    hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR, posixio_open(nofill_mod_file.c_str(), NC_NOWRITE, 0, 0,
                                     &hint, nullptr, ncio.ptr(), nullptr));
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 0, 32, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, hdr.data(), hdr.size()));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, 0));
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 384, 200, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, seg2.data(), seg2.size()));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 384, 0));
    ASSERT_EQ(NC_NOERR, ncio.close(0));
  }

  // Exercise lower hit, upper page-in, upper hit without fault, upper hit with
  // and without RGN_MODIFIED, page-below with and without RGN_MODIFIED,
  // non-overlapping jumps, and EOF short read / extent expansion.
  TempFile file("posixio_px_get_cache.nc");
  std::vector<unsigned char> expected = MakePattern(2048, 7);
  {
    ScopedNcio ncio;
    size_t hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR, posixio_create(file.c_str(), NC_CLOBBER, 2048, 0, 0,
                                       &hint, nullptr, ncio.ptr(), nullptr));
    for (size_t off = 0; off < expected.size(); off += kMinBlockSize) {
      void* vp = nullptr;
      ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), static_cast<off_t>(off),
                                   kMinBlockSize, RGN_WRITE, &vp));
      std::memcpy(vp, expected.data() + off, kMinBlockSize);
      ASSERT_EQ(NC_NOERR,
                ncio_rel(ncio.get(), static_cast<off_t>(off), RGN_MODIFIED));
    }
    ASSERT_EQ(NC_NOERR, ncio.close(0));
  }

  {
    ScopedNcio ncio;
    size_t hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR, posixio_open(file.c_str(), NC_WRITE, 0, 0, &hint,
                                     nullptr, ncio.ptr(), nullptr));
    void* vp = nullptr;

    // 1. Initial pgin [0..256).
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 0, 100, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, expected.data(), 100));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, 0));

    // 2. Hit in lower block (blkextent <= bf_extent).
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 50, 100, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, expected.data() + 50, 100));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 50, 0));

    // 3. Hit in lower block needing upper half paged in (blkextent >
    // bf_extent).
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 128, 256, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, expected.data() + 128, 256));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 128, 0));

    // 4. Hit in upper half with no fault needed (blkextent == blksz).
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 300, 100, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, expected.data() + 300, 100));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 300, 0));

    // 5. Hit in upper half shifting [256..512) down without RGN_MODIFIED.
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 400, 200, RGN_WRITE, &vp));
    EXPECT_EQ(0, std::memcmp(vp, expected.data() + 400, 200));
    const std::vector<unsigned char> patch1 = MakePattern(200, 55);
    std::memcpy(vp, patch1.data(), patch1.size());
    std::copy(patch1.begin(), patch1.end(), expected.begin() + 400);
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 400, RGN_MODIFIED));

    // 6. Hit in upper half with RGN_MODIFIED -> pages out [256..512).
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 650, 200, RGN_WRITE, &vp));
    EXPECT_EQ(0, std::memcmp(vp, expected.data() + 650, 200));
    const std::vector<unsigned char> patch2 = MakePattern(200, 66);
    std::memcpy(vp, patch2.data(), patch2.size());
    std::copy(patch2.begin(), patch2.end(), expected.begin() + 650);
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 650, RGN_MODIFIED));

    // 7. Page below with bf_cnt > blksz and RGN_MODIFIED -> pages out upper.
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 256, 128, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, expected.data() + 256, 128));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 256, 0));

    // 8. Non-overlapping jump forward with RGN_MODIFIED -> pages out
    // [256..768).
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 1280, 300, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, expected.data() + 1280, 300));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 1280, 0));

    // 9. Page below with bf_cnt > blksz and unmodified buffer.
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 1024, 128, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, expected.data() + 1024, 128));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 1024, 0));

    // 10. Non-overlapping jump to single block, then page below (bf_cnt ==
    // blksz).
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 1792, 64, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, expected.data() + 1792, 64));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 1792, 0));

    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 1536, 64, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, expected.data() + 1536, 64));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 1536, 0));

    // 11. Jump back to block 0, then request past EOF (short read + extend).
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 0, 64, 0, &vp));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, 0));

    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 2040, 100, RGN_WRITE, &vp));
    EXPECT_EQ(0, std::memcmp(vp, expected.data() + 2040, 8));
    for (size_t i = 8; i < 100; ++i) {
      EXPECT_EQ(0, static_cast<unsigned char*>(vp)[i]);
    }
    const std::vector<unsigned char> tail = MakePattern(100, 77);
    std::memcpy(vp, tail.data(), tail.size());
    expected.resize(2140);
    std::copy(tail.begin(), tail.end(), expected.begin() + 2040);
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 2040, RGN_MODIFIED));

    ASSERT_EQ(NC_NOERR, ncio.close(0));
  }

  // Reopen read-only and verify full contents after flush.
  ScopedNcio verify;
  size_t hint = kMinBlockSize;
  ASSERT_EQ(NC_NOERR, posixio_open(file.c_str(), NC_NOWRITE, 0, 0, &hint,
                                   nullptr, verify.ptr(), nullptr));
  off_t final_size = -1;
  ASSERT_EQ(NC_NOERR, ncio_filesize(verify.get(), &final_size));
  EXPECT_EQ(static_cast<off_t>(expected.size()), final_size);
  for (size_t off = 0; off < expected.size(); off += kMinBlockSize) {
    const size_t chunk = std::min(kMinBlockSize, expected.size() - off);
    void* vp = nullptr;
    ASSERT_EQ(NC_NOERR,
              ncio_get(verify.get(), static_cast<off_t>(off), chunk, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, expected.data() + off, chunk));
    ASSERT_EQ(NC_NOERR, ncio_rel(verify.get(), static_cast<off_t>(off), 0));
  }
  EXPECT_EQ(NC_NOERR, verify.close(0));
}

TEST(PosixioTest, PxSyncBehaviorForModifiedReadOnlyAndUnmodifiedWrite) {
  TempFile file("posixio_sync.nc");
  const std::vector<unsigned char> initial = MakePattern(32, 91);
  {
    ScopedNcio ncio;
    size_t hint = kMinBlockSize;
    void* vp = nullptr;
    ASSERT_EQ(NC_NOERR, posixio_create(file.c_str(), NC_CLOBBER, 512, 0, 32,
                                       &hint, nullptr, ncio.ptr(), &vp));
    // pxp->pos is 0 == pxp->bf_offset: sync exercises px_pgout when *posp ==
    // offset (skipping lseek).
    std::memcpy(vp, initial.data(), initial.size());
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, RGN_MODIFIED));
    ASSERT_EQ(NC_NOERR, ncio_sync(ncio.get()));

    // Acquire RGN_WRITE then release with 0 (not RGN_MODIFIED): sync hits the
    // branch where RGN_WRITE is set and RGN_MODIFIED is not.
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 0, 16, RGN_WRITE, &vp));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, 0));
    ASSERT_EQ(NC_NOERR, ncio_sync(ncio.get()));
    ASSERT_EQ(NC_NOERR, ncio.close(0));
  }

  ScopedNcio ro;
  size_t hint = kMinBlockSize;
  ASSERT_EQ(NC_NOERR, posixio_open(file.c_str(), NC_NOWRITE, 0, 0, &hint,
                                   nullptr, ro.ptr(), nullptr));
  void* vp = nullptr;
  ASSERT_EQ(NC_NOERR, ncio_get(ro.get(), 0, 16, 0, &vp));
  EXPECT_EQ(0, std::memcmp(vp, initial.data(), 16));
  ASSERT_EQ(NC_NOERR, ncio_rel(ro.get(), 0, 0));

  // Modify bytes [0..16) externally on disk.
  const std::vector<unsigned char> updated = MakePattern(16, 123);
  {
    ScopedNcio rw;
    size_t rw_hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR, posixio_open(file.c_str(), NC_WRITE, 0, 0, &rw_hint,
                                     nullptr, rw.ptr(), nullptr));
    void* rw_vp = nullptr;
    ASSERT_EQ(NC_NOERR, ncio_get(rw.get(), 0, 16, RGN_WRITE, &rw_vp));
    std::memcpy(rw_vp, updated.data(), updated.size());
    ASSERT_EQ(NC_NOERR, ncio_rel(rw.get(), 0, RGN_MODIFIED));
    ASSERT_EQ(NC_NOERR, rw.close(0));
  }

  // Syncing the read-only handle invalidates its buffer so the next get reads
  // the updated bytes from disk.
  ASSERT_EQ(NC_NOERR, ncio_sync(ro.get()));
  ASSERT_EQ(NC_NOERR, ncio_get(ro.get(), 0, 16, 0, &vp));
  EXPECT_EQ(0, std::memcmp(vp, updated.data(), 16));
  ASSERT_EQ(NC_NOERR, ncio_rel(ro.get(), 0, 0));
  EXPECT_EQ(NC_NOERR, ro.close(0));
}

TEST(PosixioTest, PxMoveCoversSmallAndDoubleBufferedGrowAndShrink) {
  TempFile file("posixio_move.nc");
  std::vector<unsigned char> data = MakePattern(2048, 33);
  {
    ScopedNcio ncio;
    size_t hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR, posixio_create(file.c_str(), NC_CLOBBER, 2048, 0, 0,
                                       &hint, nullptr, ncio.ptr(), nullptr));
    for (size_t off = 0; off < data.size(); off += kMinBlockSize) {
      void* vp = nullptr;
      ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), static_cast<off_t>(off),
                                   kMinBlockSize, RGN_WRITE, &vp));
      std::memcpy(vp, data.data() + off, kMinBlockSize);
      ASSERT_EQ(NC_NOERR,
                ncio_rel(ncio.get(), static_cast<off_t>(off), RGN_MODIFIED));
    }

    // 1. NOOP move (to == from).
    EXPECT_EQ(NC_NOERR, ncio_move(ncio.get(), 64, 64, 128, 0));

    // 2. Small overlapping moves (extent <= blksz): growing and shrinking.
    ASSERT_EQ(NC_NOERR, ncio_move(ncio.get(), 48, 16, 64, 0));
    std::memmove(data.data() + 48, data.data() + 16, 64);

    ASSERT_EQ(NC_NOERR, ncio_move(ncio.get(), 16, 48, 64, 0));
    std::memmove(data.data() + 16, data.data() + 48, 64);

    // 3. Large double-buffered growing move (extent > blksz), followed by
    // ncio_get to exercise slave reclamation inside ncio_px_get.
    ASSERT_EQ(NC_NOERR, ncio_move(ncio.get(), 400, 64, 650, 0));
    std::memmove(data.data() + 400, data.data() + 64, 650);

    void* vp = nullptr;
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 400, 128, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, data.data() + 400, 128));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 400, 0));

    // 4. Large double-buffered shrinking move (extent > blksz), then close
    // directly so ncio_px_freepvt frees the slave buffer.
    ASSERT_EQ(NC_NOERR, ncio_move(ncio.get(), 64, 400, 650, 0));
    std::memmove(data.data() + 64, data.data() + 400, 650);

    ASSERT_EQ(NC_NOERR, ncio.close(0));
  }

  // Verify moved contents persisted on disk.
  {
    ScopedNcio verify;
    size_t hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR, posixio_open(file.c_str(), NC_WRITE, 0, 0, &hint,
                                     nullptr, verify.ptr(), nullptr));
    for (size_t off = 0; off < data.size(); off += kMinBlockSize) {
      void* vp = nullptr;
      ASSERT_EQ(NC_NOERR, ncio_get(verify.get(), static_cast<off_t>(off),
                                   kMinBlockSize, 0, &vp));
      EXPECT_EQ(0, std::memcmp(vp, data.data() + off, kMinBlockSize));
      ASSERT_EQ(NC_NOERR, ncio_rel(verify.get(), static_cast<off_t>(off), 0));
    }

    // 5. Negative move error paths (small, shrinking double-buffer, growing
    // double-buffer). Sync first so RGN_MODIFIED is clear and bf_refcount
    // invariants hold on close after px_double_buffer's second px_get fails.
    ASSERT_EQ(NC_NOERR, ncio_sync(verify.get()));
    EXPECT_EQ(NC_ENOTNC, ncio_move(verify.get(), 16, -8, 16, 0));
    EXPECT_EQ(NC_ENOTNC, ncio_move(verify.get(), -1, 400, 10, 0));
    EXPECT_EQ(NC_ENOTNC, ncio_move(verify.get(), 400, -1, 10, 0));
    EXPECT_EQ(NC_NOERR, verify.close(0));
  }
}

TEST(PosixioTest, ShareModeSpxGetRelMoveSyncAndClose) {
  TempFile file("posixio_share.nc");
  std::vector<unsigned char> data = MakePattern(512, 42);

  {
    ScopedNcio ncio;
    size_t hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR,
              posixio_create(file.c_str(), NC_CLOBBER | NC_SHARE, 0, 0, 0,
                             &hint, nullptr, ncio.ptr(), nullptr));
    void* vp = nullptr;
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 0, 32, RGN_WRITE, &vp));
    std::memcpy(vp, data.data(), 32);
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, RGN_MODIFIED));

    // Request larger than initial sizehint triggers buffer reallocation.
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 0, 512, RGN_WRITE, &vp));
    std::memcpy(vp, data.data(), 512);
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, RGN_MODIFIED));

    // Read-only get/rel without RGN_MODIFIED.
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 0, 64, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, data.data(), 64));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, 0));

    // Move NOOP, grow, shrink, and invalid offset error.
    EXPECT_EQ(NC_NOERR, ncio_move(ncio.get(), 64, 64, 32, 0));
    ASSERT_EQ(NC_NOERR, ncio_move(ncio.get(), 256, 0, 128, 0));
    std::memmove(data.data() + 256, data.data(), 128);
    ASSERT_EQ(NC_NOERR, ncio_move(ncio.get(), 32, 256, 128, 0));
    std::memmove(data.data() + 32, data.data() + 256, 128);
    // Avoid offset == -1 because px_pgin compares lseek() against offset (-1).
    EXPECT_EQ(EINVAL, ncio_move(ncio.get(), -8, 10, 8, 0));

    EXPECT_EQ(NC_NOERR, ncio_sync(ncio.get()));
    ncio_closefunc* const spx_close_fn = ncio.get()->close;
    ASSERT_EQ(NC_NOERR, ncio.close(0));
    EXPECT_EQ(EINVAL, spx_close_fn(nullptr, 0));
  }

  {
    ScopedNcio ro;
    size_t hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR, posixio_open(file.c_str(), NC_NOWRITE | NC_SHARE, 0, 0,
                                     &hint, nullptr, ro.ptr(), nullptr));
    void* vp = nullptr;
    EXPECT_EQ(EPERM, ncio_get(ro.get(), 0, 16, RGN_WRITE, &vp));
    EXPECT_EQ(EPERM, ncio_move(ro.get(), 16, 0, 8, 0));

    ASSERT_EQ(NC_NOERR, ncio_get(ro.get(), 0, 64, 0, &vp));
    EXPECT_EQ(0, std::memcmp(vp, data.data(), 64));
    EXPECT_EQ(EPERM, ncio_rel(ro.get(), 0, RGN_MODIFIED));
    EXPECT_EQ(NC_NOERR, ro.close(0));
  }

  // Test spx close with doUnlink = 1 and spx unwind when fd < 0.
  TempFile unlinked("posixio_share_unlink.nc");
  ScopedNcio unlink_ncio;
  ScopedNcio err_ncio;
  size_t hint = kMinBlockSize;
  EXPECT_EQ(ENOENT, posixio_open(unlinked.c_str(), NC_NOWRITE | NC_SHARE, 0, 0,
                                 &hint, nullptr, err_ncio.ptr(), nullptr));
  ASSERT_EQ(NC_NOERR,
            posixio_create(unlinked.c_str(), NC_CLOBBER | NC_SHARE, 0, 0, 0,
                           &hint, nullptr, unlink_ncio.ptr(), nullptr));
  EXPECT_TRUE(std::filesystem::exists(unlinked.path()));
  EXPECT_EQ(EEXIST,
            posixio_create(unlinked.c_str(), NC_NOCLOBBER | NC_SHARE, 0, 0, 0,
                           &hint, nullptr, err_ncio.ptr(), nullptr));
  ASSERT_EQ(NC_NOERR, unlink_ncio.close(1));
  EXPECT_FALSE(std::filesystem::exists(unlinked.path()));
}

TEST(PosixioTest, PadLengthGrowsShortFileAndNoopsWhenAlreadyLarge) {
  TempFile file("posixio_pad.nc");
  ScopedNcio ncio;
  size_t hint = kMinBlockSize;
  ASSERT_EQ(NC_NOERR, posixio_create(file.c_str(), NC_CLOBBER, 64, 0, 0, &hint,
                                     nullptr, ncio.ptr(), nullptr));
  ASSERT_EQ(NC_NOERR, ncio.close(0));

  hint = kMinBlockSize;
  ASSERT_EQ(NC_NOERR, posixio_open(file.c_str(), NC_WRITE, 0, 0, &hint, nullptr,
                                   ncio.ptr(), nullptr));
  ASSERT_EQ(NC_NOERR, ncio_pad_length(ncio.get(), 32));
  off_t sz = -1;
  ASSERT_EQ(NC_NOERR, ncio_filesize(ncio.get(), &sz));
  EXPECT_EQ(64, sz);

  ASSERT_EQ(NC_NOERR, ncio_pad_length(ncio.get(), 64));
  ASSERT_EQ(NC_NOERR, ncio_filesize(ncio.get(), &sz));
  EXPECT_EQ(64, sz);

  ASSERT_EQ(NC_NOERR, ncio_pad_length(ncio.get(), 512));
  ASSERT_EQ(NC_NOERR, ncio_filesize(ncio.get(), &sz));
  EXPECT_EQ(512, sz);
  EXPECT_EQ(NC_NOERR, ncio.close(0));
}

TEST(PosixioTest, InjectedFdErrorsExerciseKernelIoFailurePaths) {
  TempFile data_file("posixio_fd_err_data.nc");
  TempFile ro_file("posixio_fd_err_ro.nc");
  TempFile wo_file("posixio_fd_err_wo.nc");

  {
    ScopedNcio init_ncio;
    size_t hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR,
              posixio_create(data_file.c_str(), NC_CLOBBER, 2048, 0, 0, &hint,
                             nullptr, init_ncio.ptr(), nullptr));
    ASSERT_EQ(NC_NOERR, init_ncio.close(0));
  }

  ScopedFd ro_fd(::open(ro_file.c_str(), O_RDONLY | O_CREAT | O_TRUNC, 0600));
  ASSERT_GE(ro_fd.get(), 0);
  ScopedFd wo_fd(::open(wo_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600));
  ASSERT_GE(wo_fd.get(), 0);

  int pipe_fds[2] = {-1, -1};
  ASSERT_EQ(0, ::pipe(pipe_fds));
  ScopedFd pipe_r(pipe_fds[0]);
  ScopedFd pipe_w(pipe_fds[1]);

  // 1. filesize and fgrow2 error paths via pad_length.
  {
    ScopedNcio ncio;
    size_t hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR, posixio_open(data_file.c_str(), NC_WRITE, 0, 0, &hint,
                                     nullptr, ncio.ptr(), nullptr));
    const int orig_fd = ncio.get()->fd;
    absl::Cleanup restore_fd([&] { ncio.get()->fd = orig_fd; });

    ncio.get()->fd = -1;
    off_t sz = 0;
    EXPECT_EQ(EBADF, ncio_filesize(ncio.get(), &sz));
    EXPECT_EQ(EBADF, ncio_pad_length(ncio.get(), 4096));

    ncio.get()->fd = pipe_w.get();
    EXPECT_EQ(ESPIPE, ncio_pad_length(ncio.get(), 4096));

    ncio.get()->fd = ro_fd.get();
    EXPECT_EQ(EBADF, ncio_pad_length(ncio.get(), 4096));
  }

  // 2. px_pgin failure paths (lseek failure on pipe, read failure on O_WRONLY,
  // and pos mismatch check when fd == -1 across D2/E/F/G).
  {
    ScopedNcio ncio;
    size_t hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR, posixio_open(data_file.c_str(), NC_NOWRITE, 0, 0, &hint,
                                     nullptr, ncio.ptr(), nullptr));
    const int orig_fd = ncio.get()->fd;
    void* vp = nullptr;

    {
      absl::Cleanup restore_fd([&] { ncio.get()->fd = orig_fd; });
      ncio.get()->fd = pipe_r.get();
      EXPECT_EQ(ESPIPE, ncio_get(ncio.get(), 0, 16, 0, &vp));
    }

    {
      absl::Cleanup restore_fd([&] {
        const off_t after = ::lseek(wo_fd.get(), 0, SEEK_CUR);
        ncio.get()->fd = orig_fd;
        if (after >= 0) {
          static_cast<void>(::lseek(orig_fd, after, SEEK_SET));
        }
      });
      ncio.get()->fd = wo_fd.get();
      EXPECT_EQ(EBADF, ncio_get(ncio.get(), 0, 16, 0, &vp));
    }

    // Read block 0 (sets pos = 256), then fail in D2 and G.
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 0, 16, 0, &vp));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, 0));
    {
      absl::Cleanup restore_fd([&] { ncio.get()->fd = orig_fd; });
      ncio.get()->fd = -1;
      EXPECT_EQ(EBADF, ncio_get(ncio.get(), 128, 256, 0, &vp));
      EXPECT_EQ(EBADF, ncio_get(ncio.get(), 1024, 16, 0, &vp));
    }

    // Read [0..300) (bf_offset = 0, bf_extent = 512), then fail in E upper
    // pgin.
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 0, 300, 0, &vp));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, 0));
    {
      absl::Cleanup restore_fd([&] { ncio.get()->fd = orig_fd; });
      ncio.get()->fd = -1;
      EXPECT_EQ(EBADF, ncio_get(ncio.get(), 384, 200, 0, &vp));
    }

    // Read [256..300) (bf_offset = 256), then fail in F lower pgin.
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 256, 44, 0, &vp));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 256, 0));
    {
      absl::Cleanup restore_fd([&] { ncio.get()->fd = orig_fd; });
      ncio.get()->fd = -1;
      EXPECT_EQ(EBADF, ncio_get(ncio.get(), 0, 16, 0, &vp));
    }

    ASSERT_EQ(NC_NOERR, ncio.close(0));
  }

  // 3. px_pgout write failure paths across sync, pad_length, E2, F1, G1, and
  // E3.
  auto inject_ro_pgout = [&](ncio* n, auto&& fn) {
    const int orig_fd = n->fd;
    const off_t cur = ::lseek(orig_fd, 0, SEEK_CUR);
    ASSERT_GE(cur, 0);
    ASSERT_EQ(cur, ::lseek(ro_fd.get(), cur, SEEK_SET));
    n->fd = ro_fd.get();
    absl::Cleanup restore([&] {
      const off_t after = ::lseek(ro_fd.get(), 0, SEEK_CUR);
      n->fd = orig_fd;
      if (after >= 0) {
        static_cast<void>(::lseek(orig_fd, after, SEEK_SET));
      }
    });
    fn();
  };

  {
    ScopedNcio ncio;
    size_t hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR, posixio_open(data_file.c_str(), NC_WRITE, 0, 0, &hint,
                                     nullptr, ncio.ptr(), nullptr));
    void* vp = nullptr;
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 0, 300, RGN_WRITE, &vp));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, RGN_MODIFIED));

    inject_ro_pgout(ncio.get(), [&] {
      EXPECT_EQ(EBADF, ncio_sync(ncio.get()));
      EXPECT_EQ(EBADF, ncio_pad_length(ncio.get(), 4096));
      EXPECT_EQ(EBADF, ncio_get(ncio.get(), 384, 200, 0, &vp));
      EXPECT_EQ(EBADF, ncio_get(ncio.get(), 1024, 16, 0, &vp));
    });

    ASSERT_EQ(NC_NOERR, ncio_sync(ncio.get()));
    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 256, 300, RGN_WRITE, &vp));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 256, RGN_MODIFIED));
    inject_ro_pgout(ncio.get(), [&] {
      EXPECT_EQ(EBADF, ncio_get(ncio.get(), 0, 16, 0, &vp));
    });
    ASSERT_EQ(NC_NOERR, ncio.close(0));
  }

  {
    ScopedNcio ncio;
    size_t hint = kMinBlockSize;
    void* vp = nullptr;
    ASSERT_EQ(NC_NOERR, posixio_create(data_file.c_str(), NC_CLOBBER, 2048, 0,
                                       16, &hint, nullptr, ncio.ptr(), &vp));
    ASSERT_EQ(NC_NOERR, ncio_rel(ncio.get(), 0, RGN_MODIFIED));
    inject_ro_pgout(ncio.get(), [&] {
      EXPECT_EQ(EBADF, ncio_get(ncio.get(), 384, 200, 0, &vp));
    });
    ASSERT_EQ(NC_NOERR, ncio.close(0));
  }

  // 4. ncio_spx_get and ncio_spx_rel kernel I/O error paths.
  {
    ScopedNcio ncio;
    size_t hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR, posixio_open(data_file.c_str(), NC_WRITE | NC_SHARE, 0,
                                     0, &hint, nullptr, ncio.ptr(), nullptr));
    const int orig_fd = ncio.get()->fd;
    void* vp = nullptr;
    {
      absl::Cleanup restore([&] { ncio.get()->fd = orig_fd; });
      ncio.get()->fd = pipe_r.get();
      EXPECT_EQ(ESPIPE, ncio_get(ncio.get(), 0, 16, 0, &vp));
    }

    ASSERT_EQ(NC_NOERR, ncio_get(ncio.get(), 0, 16, RGN_WRITE, &vp));
    inject_ro_pgout(ncio.get(), [&] {
      EXPECT_EQ(EBADF, ncio_rel(ncio.get(), 0, RGN_MODIFIED));
    });
    EXPECT_EQ(NC_NOERR, ncio.close(0));
  }
}

TEST(PosixioTest, PublicNetcdf3ApiDiskOperationsRoundTrip) {
  TempFile nc_file("posixio_api_roundtrip.nc");
  size_t hint = kMinBlockSize;

  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc__create(nc_file.c_str(), NC_CLOBBER | NC_64BIT_OFFSET, 0,
                         &hint, nc.ptr()));
    int old_fill = -1;
    ASSERT_EQ(NC_NOERR, nc_set_fill(nc.get(), NC_NOFILL, &old_fill));
    EXPECT_EQ(NC_FILL, old_fill);

    int dim_x = -1;
    int dim_rec = -1;
    int var_a = -1;
    int var_r = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 200, &dim_x));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "rec", NC_UNLIMITED, &dim_rec));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "a", NC_INT, 1, &dim_x, &var_a));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "r", NC_INT, 1, &dim_rec, &var_r));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    // Write sparse tail values in NC_NOFILL mode so close invokes pad_length.
    const size_t idx = 1;
    const int32_t rec_val = 42;
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(nc.get(), var_r, &idx, &rec_val));
    ASSERT_EQ(NC_NOERR, nc_sync(nc.get()));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Reopen with nc__open, redefine to expand header and trigger ncio_move.
  {
    ScopedNcFile nc;
    size_t open_hint = kMinBlockSize;
    ASSERT_EQ(NC_NOERR,
              nc__open(nc_file.c_str(), NC_WRITE, &open_hint, nc.ptr()));
    ASSERT_EQ(NC_NOERR, nc_redef(nc.get()));
    const std::string long_attr(400, 'z');
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), NC_GLOBAL, "pad_hdr",
                                        long_attr.size(), long_attr.data()));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    int var_r = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(nc.get(), "r", &var_r));
    const size_t idx = 1;
    int32_t got = -1;
    ASSERT_EQ(NC_NOERR, nc_get_var1_int(nc.get(), var_r, &idx, &got));
    EXPECT_EQ(42, got);
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // NC_SHARE create + concurrent NC_SHARE reader + nc_delete.
  TempFile share_file("posixio_api_share.nc");
  {
    ScopedNcFile writer;
    ASSERT_EQ(NC_NOERR, nc_create(share_file.c_str(), NC_CLOBBER | NC_SHARE,
                                  writer.ptr()));
    int dim_t = -1;
    int var_v = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(writer.get(), "t", NC_UNLIMITED, &dim_t));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(writer.get(), "v", NC_INT, 1, &dim_t, &var_v));
    ASSERT_EQ(NC_NOERR, nc_enddef(writer.get()));

    const size_t idx0 = 0;
    const int32_t v0 = 101;
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(writer.get(), var_v, &idx0, &v0));
    ASSERT_EQ(NC_NOERR, nc_sync(writer.get()));

    ScopedNcFile reader;
    ASSERT_EQ(NC_NOERR,
              nc_open(share_file.c_str(), NC_NOWRITE | NC_SHARE, reader.ptr()));
    int r_var = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(reader.get(), "v", &r_var));
    int32_t got0 = -1;
    ASSERT_EQ(NC_NOERR, nc_get_var1_int(reader.get(), r_var, &idx0, &got0));
    EXPECT_EQ(101, got0);

    // Writer redefines and moves data under NC_SHARE.
    ASSERT_EQ(NC_NOERR, nc_redef(writer.get()));
    const std::string share_attr(128, 's');
    ASSERT_EQ(NC_NOERR, nc_put_att_text(writer.get(), NC_GLOBAL, "meta",
                                        share_attr.size(), share_attr.data()));
    ASSERT_EQ(NC_NOERR, nc_enddef(writer.get()));
    ASSERT_EQ(NC_NOERR, nc_sync(writer.get()));

    ASSERT_EQ(NC_NOERR, nc_sync(reader.get()));
    int32_t got_after_move = -1;
    ASSERT_EQ(NC_NOERR,
              nc_get_var1_int(reader.get(), r_var, &idx0, &got_after_move));
    EXPECT_EQ(101, got_after_move);

    ASSERT_EQ(NC_NOERR, reader.close());
    ASSERT_EQ(NC_NOERR, writer.close());
  }

  ASSERT_EQ(NC_NOERR, nc_delete(share_file.c_str()));
  EXPECT_FALSE(std::filesystem::exists(share_file.path()));

  // nc_abort during initial define unlinks the file.
  TempFile abort_file("posixio_api_abort.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(abort_file.c_str(), NC_CLOBBER, nc.ptr()));
    EXPECT_TRUE(std::filesystem::exists(abort_file.path()));
    ASSERT_EQ(NC_NOERR, nc.abort());
    EXPECT_FALSE(std::filesystem::exists(abort_file.path()));
  }
}

}  // namespace
}  // namespace netcdf
