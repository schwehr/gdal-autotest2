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

// Unit tests for //third_party/netcdf/libhdf5/nc4mem.c (HDF5 memory files).
//
// Covers in-memory netCDF-4 creation, opening (locked/unlocked, read-only/RW),
// buffer growth/ownership transfer via nc_open_memio/nc_close_memio, corrupt
// image rejection, and internal NC4_open_image_file/NC4_create_image_file
// error checks.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libhdf5/nc4mem.c

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/libhdf5/nc4mem_shim.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::ElementsAre;

constexpr std::array<unsigned char, 8> kHdf5Magic = {
    0x89, 'H', 'D', 'F', '\r', '\n', 0x1a, '\n',
};

ScopedMemio BuildSampleNetcdf4Image() {
  ScopedNcFile nc;
  if (nc_create_mem("nc4mem_sample.nc", NC_NETCDF4, 4096, nc.ptr()) !=
      NC_NOERR) {
    return ScopedMemio();
  }
  int dimid = -1;
  int varid = -1;
  if (nc_def_dim(nc, "x", 4, &dimid) != NC_NOERR ||
      nc_def_var(nc, "vals", NC_INT, 1, &dimid, &varid) != NC_NOERR) {
    return ScopedMemio();
  }
  const std::array<int, 4> kInit = {10, 20, 30, 40};
  if (nc_enddef(nc) != NC_NOERR ||
      nc_put_var_int(nc, varid, kInit.data()) != NC_NOERR) {
    return ScopedMemio();
  }
  ScopedMemio out;
  if (nc_close_memio(nc.release(), out.get()) != NC_NOERR) {
    return ScopedMemio();
  }
  return out;
}

// Builds a serialized netCDF-4 image created with nc_create() so it carries
// HDF5 creation-order tracking and can be reopened read-write with
// nc_open_memio(..., NC_WRITE, ...). Images built directly with nc_create_mem()
// omit fcpl creation-order tracking in NC4_image_init() and reject NC_WRITE
// with NC_ECANTWRITE.
std::vector<unsigned char> BuildWritableNetcdf4Image(
    std::string_view temp_filename) {
  TempFile path(temp_filename);
  ScopedNcFile nc;
  if (nc_create(path.c_str(), NC_NETCDF4 | NC_CLOBBER, nc.ptr()) != NC_NOERR) {
    return {};
  }
  int dimid = -1;
  int varid = -1;
  if (nc_def_dim(nc, "x", 4, &dimid) != NC_NOERR ||
      nc_def_var(nc, "vals", NC_INT, 1, &dimid, &varid) != NC_NOERR) {
    return {};
  }
  const std::array<int, 4> kInit = {10, 20, 30, 40};
  if (nc_enddef(nc) != NC_NOERR ||
      nc_put_var_int(nc, varid, kInit.data()) != NC_NOERR ||
      nc.close() != NC_NOERR) {
    return {};
  }

  std::ifstream input(path.c_str(), std::ios::binary | std::ios::ate);
  if (!input.is_open()) {
    return {};
  }
  const std::streamoff end_pos = input.tellg();
  if (end_pos <= 0) {
    return {};
  }
  std::vector<unsigned char> bytes(static_cast<size_t>(end_pos));
  input.seekg(0, std::ios::beg);
  if (!input.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()))) {
    return {};
  }
  return bytes;
}

TEST(Nc4MemTest, CreateMemCreatesNetcdf4ImageAndOpenMemReadsItBack) {
  ScopedMemio image = BuildSampleNetcdf4Image();
  ASSERT_NE(image.get()->memory, nullptr);
  ASSERT_GT(image.size(), 8U);

  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open_mem("nc4mem_open_ro.nc", NC_NOWRITE, image.size(),
                                  image.get()->memory, ro.ptr()));

  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "vals", &varid));
  std::vector<int> actual(4, -999);
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, varid, actual.data()));
  EXPECT_THAT(actual, ElementsAre(10, 20, 30, 40));

  // Verify read-only rejection on in-memory dataset opened with NC_NOWRITE.
  EXPECT_EQ(NC_EPERM, nc_redef(ro));
}

TEST(Nc4MemTest, OpenMemioUnlockedReadWriteTakesOwnershipAndGrowsBuffer) {
  std::vector<unsigned char> base =
      BuildWritableNetcdf4Image("nc4mem_unlocked_rw_seed.nc");
  ASSERT_FALSE(base.empty());

  void* rw_buf = std::malloc(base.size());
  ASSERT_NE(rw_buf, nullptr);
  std::memcpy(rw_buf, base.data(), base.size());

  NC_memio in_memio{base.size(), rw_buf, 0};
  // Once netCDF takes ownership of unlocked RW memory, it clears
  // in_memio.memory to nullptr, making std::free(nullptr) a no-op.
  auto free_unclaimed =
      absl::MakeCleanup([&in_memio]() { std::free(in_memio.memory); });
  ScopedNcFile rw;
  ASSERT_EQ(NC_NOERR,
            nc_open_memio("nc4mem_open_rw.nc", NC_WRITE, &in_memio, rw.ptr()));
  // Unlocked NC_WRITE transfers buffer ownership into netCDF.
  EXPECT_EQ(nullptr, in_memio.memory);
  EXPECT_EQ(0U, in_memio.size);

  int big_dimid = -1;
  int big_varid = -1;
  constexpr size_t kGrowCount = 24576;
  ASSERT_EQ(NC_NOERR, nc_def_dim(rw, "big", kGrowCount, &big_dimid));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(rw, "payload", NC_INT, 1, &big_dimid, &big_varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(rw));

  std::vector<int> data(kGrowCount, 77);
  ASSERT_EQ(NC_NOERR, nc_put_var_int(rw, big_varid, data.data()));

  ScopedMemio out_memio;
  ASSERT_EQ(NC_NOERR, nc_close_memio(rw.release(), out_memio.get()));
  ASSERT_NE(out_memio.get()->memory, nullptr);
  EXPECT_GT(out_memio.size(), base.size());

  ScopedNcFile verify;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("nc4mem_verify_rw.nc", NC_NOWRITE, out_memio.size(),
                        out_memio.get()->memory, verify.ptr()));
  int v_big = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(verify, "payload", &v_big));
  std::vector<int> read_back(kGrowCount, -1);
  ASSERT_EQ(NC_NOERR, nc_get_var_int(verify, v_big, read_back.data()));
  EXPECT_EQ(read_back, data);
}

TEST(Nc4MemTest, OpenMemioUnlockedReadOnlyAutoFreesBufferOnClose) {
  ScopedMemio base = BuildSampleNetcdf4Image();
  ASSERT_NE(base.get()->memory, nullptr);
  ASSERT_GT(base.size(), 0U);

  void* heap_buf = std::malloc(base.size());
  ASSERT_NE(heap_buf, nullptr);
  std::memcpy(heap_buf, base.get()->memory, base.size());

  // NC_NOWRITE without NC_MEMIO_LOCKED exercises locked == 0 && no_write == 1;
  // netCDF takes ownership once NC4_open_image_file enters and frees heap_buf
  // inside nc_close().
  bool netcdf_owns_heap_buf = false;
  auto free_unclaimed = absl::MakeCleanup([&]() {
    if (!netcdf_owns_heap_buf) {
      std::free(heap_buf);
    }
  });
  NC_memio params{base.size(), heap_buf, 0};
  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open_memio("nc4mem_unlocked_ro.nc", NC_NOWRITE,
                                    &params, ro.ptr()));
  netcdf_owns_heap_buf = true;

  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "vals", &varid));
  std::vector<int> vals(4, 0);
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, varid, vals.data()));
  EXPECT_THAT(vals, ElementsAre(10, 20, 30, 40));
  ASSERT_EQ(NC_NOERR, ro.close());
}

TEST(Nc4MemTest, OpenMemioLockedReadWritePreservesCallerBufferPointer) {
  std::vector<unsigned char> base =
      BuildWritableNetcdf4Image("nc4mem_locked_rw_seed.nc");
  ASSERT_FALSE(base.empty());

  // Overallocate caller buffer (>= 128 KiB, and at least +64 KiB over base) so
  // HDF5's 64 KiB core-VFD allocation increment and metadata updates during
  // close stay within bounds without triggering realloc under NC_MEMIO_LOCKED.
  std::vector<unsigned char> caller_buf(
      std::max<size_t>(base.size() + 65536, 131072), 0);
  std::memcpy(caller_buf.data(), base.data(), base.size());
  NC_memio params{caller_buf.size(), caller_buf.data(), NC_MEMIO_LOCKED};
  ScopedNcFile rw_locked;
  ASSERT_EQ(NC_NOERR, nc_open_memio("nc4mem_locked_rw.nc", NC_WRITE, &params,
                                    rw_locked.ptr()));

  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(rw_locked, "vals", &varid));
  const std::array<int, 4> kUpdated = {11, 22, 33, 44};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(rw_locked, varid, kUpdated.data()));

  // Locked output aliases caller_buf.data(); do not wrap in ScopedMemio.
  NC_memio out_alias{};
  ASSERT_EQ(NC_NOERR, nc_close_memio(rw_locked.release(), &out_alias));
  EXPECT_EQ(caller_buf.data(), out_alias.memory);
  EXPECT_GT(out_alias.size, 0U);
  EXPECT_LE(out_alias.size, caller_buf.size());

  ScopedNcFile verify;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("nc4mem_locked_verify.nc", NC_NOWRITE, out_alias.size,
                        caller_buf.data(), verify.ptr()));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(verify, "vals", &varid));
  std::vector<int> actual(4, -1);
  ASSERT_EQ(NC_NOERR, nc_get_var_int(verify, varid, actual.data()));
  EXPECT_THAT(actual, ElementsAre(11, 22, 33, 44));
}

TEST(Nc4MemTest, OpenMemRejectsCorruptHdf5ImageAndBadArgs) {
  std::vector<unsigned char> corrupt(64, 0);
  std::memcpy(corrupt.data(), kHdf5Magic.data(), kHdf5Magic.size());

  ScopedNcFile bad_nc;
  EXPECT_EQ(NC_EHDFERR, nc_open_mem("corrupt_ro.nc", NC_NOWRITE, corrupt.size(),
                                    corrupt.data(), bad_nc.ptr()));

  // Unlocked NC_WRITE on corrupt HDF5 image must reclaim heap buffer on fail.
  void* corrupt_heap = std::malloc(corrupt.size());
  ASSERT_NE(corrupt_heap, nullptr);
  std::memcpy(corrupt_heap, corrupt.data(), corrupt.size());
  NC_memio corrupt_params{corrupt.size(), corrupt_heap, 0};
  auto free_unclaimed_corrupt = absl::MakeCleanup(
      [&corrupt_params]() { std::free(corrupt_params.memory); });
  EXPECT_EQ(NC_EHDFERR, nc_open_memio("corrupt_rw.nc", NC_WRITE,
                                      &corrupt_params, bad_nc.ptr()));
  EXPECT_EQ(nullptr, corrupt_params.memory);

  EXPECT_NE(NC_NOERR,
            nc_open_memio("null_params.nc", NC_NOWRITE, nullptr, bad_nc.ptr()));
  EXPECT_NE(NC_NOERR, nc_open_mem("zero_len.nc", NC_NOWRITE, 0, corrupt.data(),
                                  bad_nc.ptr()));
  EXPECT_NE(NC_NOERR,
            nc_open_mem("null_mem.nc", NC_NOWRITE, 16, nullptr, bad_nc.ptr()));
}

TEST(Nc4MemTest, ShimExercisesInvalidOpenAndCreateImageFileArgs) {
  EXPECT_EQ(NC_EINVAL, Nc4MemShimOpenImageInvalidArgs(/*null_memory=*/1,
                                                      /*zero_size=*/0));
  EXPECT_EQ(NC_EINVAL, Nc4MemShimOpenImageInvalidArgs(/*null_memory=*/0,
                                                      /*zero_size=*/1));
  EXPECT_EQ(NC_EHDFERR, Nc4MemShimCreateImageInvalidSize());
}

}  // namespace
}  // namespace netcdf
