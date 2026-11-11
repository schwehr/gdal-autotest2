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

// Unit tests for //third_party/netcdf/libsrc/ncio.c.
//
// Exercises ncio_create(), ncio_open(), and all dispatch wrapper entry points
// (ncio_get, ncio_rel, ncio_move, ncio_sync, ncio_filesize, ncio_pad_length,
// ncio_close) across POSIX, NC_DISKLESS, and NC_INMEMORY backends.

#include <sys/types.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <ios>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
#include "third_party/netcdf/libsrc/ncio.h"
}  // extern "C"

namespace netcdf {
namespace {

class ScopedNcio {
 public:
  ScopedNcio() = default;
  explicit ScopedNcio(ncio* iop, int do_unlink = 0)
      : iop_(iop), do_unlink_(do_unlink) {}

  ~ScopedNcio() { static_cast<void>(close()); }

  ScopedNcio(const ScopedNcio&) = delete;
  ScopedNcio& operator=(const ScopedNcio&) = delete;

  ncio* get() const { return iop_; }
  ncio** ptr() {
    static_cast<void>(close());
    return &iop_;
  }
  void set_unlink(int do_unlink) { do_unlink_ = do_unlink; }

  int close() {
    if (iop_ != nullptr) {
      ncio* p = iop_;
      iop_ = nullptr;
      return ncio_close(p, do_unlink_);
    }
    return NC_NOERR;
  }

 private:
  ncio* iop_ = nullptr;
  int do_unlink_ = 0;
};

TEST(NcioTest, PosixCreateOpenAndAllDispatchWrappers) {
  TempFile file("ncio_posix.dat");
  size_t sizehint = NC_SIZEHINT_DEFAULT;
  void* initial_ptr = nullptr;
  ScopedNcio io;

  ASSERT_EQ(NC_NOERR, ncio_create(file.c_str(), NC_CLOBBER, /*initialsz=*/256,
                                  /*igeto=*/0, /*igetsz=*/32, &sizehint,
                                  nullptr, io.ptr(), &initial_ptr));
  ASSERT_NE(io.get(), nullptr);
  ASSERT_NE(initial_ptr, nullptr);

  auto* initial_bytes = static_cast<uint8_t*>(initial_ptr);
  for (size_t i = 0; i < 32; ++i) {
    initial_bytes[i] = static_cast<uint8_t>(0x10U + i);
  }
  ASSERT_EQ(NC_NOERR, ncio_rel(io.get(), 0, RGN_MODIFIED));

  off_t size = 0;
  ASSERT_EQ(NC_NOERR, ncio_filesize(io.get(), &size));
  EXPECT_GE(size, off_t{256});

  // Exercise non-overlapping and overlapping ncio_move().
  ASSERT_EQ(NC_NOERR, ncio_move(io.get(), /*to=*/64, /*from=*/0, 32, 0));
  ASSERT_EQ(NC_NOERR, ncio_move(io.get(), /*to=*/80, /*from=*/64, 32, 0));

  ASSERT_EQ(NC_NOERR, ncio_pad_length(io.get(), 512));
  ASSERT_EQ(NC_NOERR, ncio_sync(io.get()));
  ASSERT_EQ(NC_NOERR, ncio_filesize(io.get(), &size));
  EXPECT_EQ(off_t{512}, size);

  void* read_ptr = nullptr;
  ASSERT_EQ(NC_NOERR, ncio_get(io.get(), 80, 32, 0, &read_ptr));
  ASSERT_NE(read_ptr, nullptr);
  const auto* read_bytes = static_cast<const uint8_t*>(read_ptr);
  for (size_t i = 0; i < 32; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(0x10U + i), read_bytes[i]);
  }
  ASSERT_EQ(NC_NOERR, ncio_rel(io.get(), 80, 0));
  ASSERT_EQ(NC_NOERR, io.close());

  // Reopen existing file read-only via posixio_open and unlink on close.
  sizehint = NC_SIZEHINT_DEFAULT;
  ASSERT_EQ(NC_NOERR, ncio_open(file.c_str(), NC_NOWRITE, 0, 0, &sizehint,
                                nullptr, io.ptr(), nullptr));
  ASSERT_EQ(NC_NOERR, ncio_filesize(io.get(), &size));
  EXPECT_EQ(off_t{512}, size);

  read_ptr = nullptr;
  ASSERT_EQ(NC_NOERR, ncio_get(io.get(), 80, 32, 0, &read_ptr));
  ASSERT_NE(read_ptr, nullptr);
  read_bytes = static_cast<const uint8_t*>(read_ptr);
  for (size_t i = 0; i < 32; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(0x10U + i), read_bytes[i]);
  }
  ASSERT_EQ(NC_NOERR, ncio_rel(io.get(), 80, 0));

  io.set_unlink(1);
  ASSERT_EQ(NC_NOERR, io.close());
  EXPECT_FALSE(std::filesystem::exists(file.path()));
}

TEST(NcioTest, PosixCreateNoClobberAndOpenMissingErrors) {
  TempFile file("ncio_errors.dat");
  size_t sizehint = NC_SIZEHINT_DEFAULT;
  ScopedNcio io;

  ASSERT_EQ(NC_NOERR, ncio_create(file.c_str(), NC_CLOBBER, 128, 0, 0,
                                  &sizehint, nullptr, io.ptr(), nullptr));
  ASSERT_EQ(NC_NOERR, io.close());

  sizehint = NC_SIZEHINT_DEFAULT;
  EXPECT_EQ(EEXIST, ncio_create(file.c_str(), NC_NOCLOBBER, 128, 0, 0,
                                &sizehint, nullptr, io.ptr(), nullptr));

  TempFile missing("ncio_missing.dat");
  sizehint = NC_SIZEHINT_DEFAULT;
  EXPECT_EQ(ENOENT, ncio_open(missing.c_str(), NC_NOWRITE, 0, 0, &sizehint,
                              nullptr, io.ptr(), nullptr));
}

TEST(NcioTest, DisklessCreateOpenAndWrappers) {
  TempFile disk_src("ncio_diskless_src.dat");
  {
    std::ofstream out(disk_src.path(), std::ios::binary);
    ASSERT_TRUE(out.good());
    std::vector<char> payload(300);
    for (size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<char>(i & 0xFFU);
    }
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    ASSERT_TRUE(out.good());
  }

  // NC_DISKLESS path inside ncio_create().
  size_t sizehint = NC_SIZEHINT_DEFAULT;
  void* initial_ptr = nullptr;
  ScopedNcio created;
  ASSERT_EQ(NC_NOERR,
            ncio_create("diskless_create.bin", NC_DISKLESS | NC_CLOBBER, 256, 0,
                        16, &sizehint, nullptr, created.ptr(), &initial_ptr));
  ASSERT_NE(created.get(), nullptr);
  ASSERT_NE(initial_ptr, nullptr);
  std::memset(initial_ptr, 0x5CU, 16);
  ASSERT_EQ(NC_NOERR, ncio_rel(created.get(), 0, RGN_MODIFIED));

  ASSERT_EQ(NC_NOERR, ncio_move(created.get(), 32, 0, 16, 0));
  ASSERT_EQ(NC_NOERR, ncio_pad_length(created.get(), 640));
  ASSERT_EQ(NC_NOERR, ncio_sync(created.get()));

  off_t sz = 0;
  ASSERT_EQ(NC_NOERR, ncio_filesize(created.get(), &sz));
  EXPECT_EQ(off_t{640}, sz);

  void* rp = nullptr;
  ASSERT_EQ(NC_NOERR, ncio_get(created.get(), 32, 16, 0, &rp));
  for (size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(0x5CU, static_cast<const uint8_t*>(rp)[i]);
  }
  ASSERT_EQ(NC_NOERR, ncio_rel(created.get(), 32, 0));
  ASSERT_EQ(NC_NOERR, created.close());

  // NC_DISKLESS path inside ncio_open() reads existing disk file.
  sizehint = NC_SIZEHINT_DEFAULT;
  ScopedNcio opened;
  ASSERT_EQ(NC_NOERR, ncio_open(disk_src.c_str(), NC_DISKLESS | NC_NOWRITE, 0,
                                0, &sizehint, nullptr, opened.ptr(), nullptr));
  ASSERT_EQ(NC_NOERR, ncio_filesize(opened.get(), &sz));
  EXPECT_EQ(off_t{300}, sz);

  rp = nullptr;
  ASSERT_EQ(NC_NOERR, ncio_get(opened.get(), 100, 64, 0, &rp));
  for (size_t i = 0; i < 64; ++i) {
    EXPECT_EQ(static_cast<uint8_t>((100U + i) & 0xFFU),
              static_cast<const uint8_t*>(rp)[i]);
  }
  ASSERT_EQ(NC_NOERR, ncio_rel(opened.get(), 100, 0));
  ASSERT_EQ(NC_NOERR, opened.close());
}

TEST(NcioTest, InmemoryCreateOpenLockedAndWritable) {
  // NC_INMEMORY path inside ncio_create().
  size_t sizehint = NC_SIZEHINT_DEFAULT;
  ScopedNcio created;
  ASSERT_EQ(NC_NOERR, ncio_create("inmem_create.bin", NC_INMEMORY, 128, 0, 0,
                                  &sizehint, nullptr, created.ptr(), nullptr));
  void* wp = nullptr;
  ASSERT_EQ(NC_NOERR, ncio_get(created.get(), 0, 24, RGN_WRITE, &wp));
  for (size_t i = 0; i < 24; ++i) {
    static_cast<uint8_t*>(wp)[i] = static_cast<uint8_t>(0x80U + i);
  }
  ASSERT_EQ(NC_NOERR, ncio_rel(created.get(), 0, RGN_MODIFIED));
  ASSERT_EQ(NC_NOERR, ncio_move(created.get(), 48, 0, 24, 0));

  void* moved_rp = nullptr;
  ASSERT_EQ(NC_NOERR, ncio_get(created.get(), 48, 24, 0, &moved_rp));
  ASSERT_NE(moved_rp, nullptr);
  for (size_t i = 0; i < 24; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(0x80U + i),
              static_cast<const uint8_t*>(moved_rp)[i]);
  }
  ASSERT_EQ(NC_NOERR, ncio_rel(created.get(), 48, 0));

  ASSERT_EQ(NC_NOERR, ncio_pad_length(created.get(), 512));
  ASSERT_EQ(NC_NOERR, ncio_sync(created.get()));
  off_t sz = 0;
  ASSERT_EQ(NC_NOERR, ncio_filesize(created.get(), &sz));
  EXPECT_EQ(off_t{512}, sz);
  ASSERT_EQ(NC_NOERR, created.close());

  // NC_INMEMORY path inside ncio_open() with caller-owned locked buffer.
  std::vector<uint8_t> caller_buf(128, 0);
  for (size_t i = 0; i < caller_buf.size(); ++i) {
    caller_buf[i] = static_cast<uint8_t>(i);
  }
  NC_memio locked_info{
      .size = caller_buf.size(),
      .memory = caller_buf.data(),
      .flags = NC_MEMIO_LOCKED,
  };
  sizehint = NC_SIZEHINT_DEFAULT;
  ScopedNcio locked_io;
  ASSERT_EQ(NC_NOERR,
            ncio_open("inmem_locked.bin", NC_INMEMORY | NC_NOWRITE, 0, 0,
                      &sizehint, &locked_info, locked_io.ptr(), nullptr));
  ASSERT_EQ(NC_NOERR, ncio_filesize(locked_io.get(), &sz));
  EXPECT_EQ(off_t{128}, sz);
  EXPECT_EQ(EPERM, ncio_pad_length(locked_io.get(), 256));

  void* rp = nullptr;
  ASSERT_EQ(NC_NOERR, ncio_get(locked_io.get(), 16, 16, 0, &rp));
  for (size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(16U + i),
              static_cast<const uint8_t*>(rp)[i]);
  }
  ASSERT_EQ(NC_NOERR, ncio_rel(locked_io.get(), 16, 0));
  ASSERT_EQ(NC_NOERR, locked_io.close());

  // NC_INMEMORY path inside ncio_open() taking ownership of mutable buffer.
  std::unique_ptr<void, decltype(&std::free)> owned_buf(std::malloc(256),
                                                        &std::free);
  ASSERT_NE(owned_buf, nullptr);
  std::memset(owned_buf.get(), 0x33, 256);
  NC_memio rw_info{
      .size = 256,
      .memory = owned_buf.get(),
      .flags = 0,
  };
  sizehint = NC_SIZEHINT_DEFAULT;
  ScopedNcio rw_io;
  const int rw_status = ncio_open("inmem_rw.bin", NC_INMEMORY | NC_WRITE, 0, 0,
                                  &sizehint, &rw_info, rw_io.ptr(), nullptr);
  // memio_open() takes ownership and sets rw_info.memory = NULL before moving
  // on.
  if (rw_info.memory == nullptr) {
    static_cast<void>(owned_buf.release());
  }
  EXPECT_EQ(nullptr, rw_info.memory);
  ASSERT_EQ(NC_NOERR, rw_status);
  ASSERT_EQ(NC_NOERR, ncio_pad_length(rw_io.get(), 320));
  ASSERT_EQ(NC_NOERR, ncio_filesize(rw_io.get(), &sz));
  EXPECT_EQ(off_t{320}, sz);
  ASSERT_EQ(NC_NOERR, rw_io.close());
}

}  // namespace
}  // namespace netcdf
