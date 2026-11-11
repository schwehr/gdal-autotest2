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

// Unit tests for //third_party/netcdf/ncdump/nciter.c.
//
// Covers contiguous, compact, and chunked iteration across 2D/3D shapes,
// leftover slices, odometer carry, unlimited-dim chunk clamping, and fatal
// NC_CHECK error paths inside forked children.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/nciter.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncdump_globals_stub.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
// clang-format on

namespace netcdf {
namespace {

constexpr std::array<std::string_view, 1> kChildArgv = {"nciter_test"};

struct NcIterGuard {
  nciter_t* iter = nullptr;

  NcIterGuard() = default;
  NcIterGuard(const NcIterGuard&) = delete;
  NcIterGuard& operator=(const NcIterGuard&) = delete;
  ~NcIterGuard() {
    if (iter != nullptr) {
      nc_free_iter(iter);
    }
  }
};

struct Slab {
  std::vector<size_t> start;
  std::vector<size_t> count;
  size_t nelems = 0;

  bool operator==(const Slab& other) const = default;
};

// Copies a 2D slab read from `ncid`/`varid` into row-major `dst`.
void ReadSlab2D(int ncid, int varid, const size_t* start, const size_t* count,
                size_t width, std::vector<int32_t>& dst) {
  std::vector<int32_t> tmp(count[0] * count[1], -1);
  ASSERT_EQ(NC_NOERR, nc_get_vara_int(ncid, varid, start, count, tmp.data()));
  for (size_t r = 0; r < count[0]; ++r) {
    for (size_t c = 0; c < count[1]; ++c) {
      dst[(start[0] + r) * width + (start[1] + c)] = tmp[r * count[1] + c];
    }
  }
}

// Copies a 3D slab read from `ncid`/`varid` into row-major `dst`.
void ReadSlab3D(int ncid, int varid, const size_t* start, const size_t* count,
                size_t dim1, size_t dim2, std::vector<int32_t>& dst) {
  std::vector<int32_t> tmp(count[0] * count[1] * count[2], -1);
  ASSERT_EQ(NC_NOERR, nc_get_vara_int(ncid, varid, start, count, tmp.data()));
  for (size_t i = 0; i < count[0]; ++i) {
    for (size_t j = 0; j < count[1]; ++j) {
      for (size_t k = 0; k < count[2]; ++k) {
        const size_t dst_idx =
            ((start[0] + i) * dim1 + (start[1] + j)) * dim2 + (start[2] + k);
        const size_t src_idx = (i * count[1] + j) * count[2] + k;
        dst[dst_idx] = tmp[src_idx];
      }
    }
  }
}

TEST(NciterTest, ContiguousAndCompactVariablesFitInSingleBuffer) {
  ncdump_stub_reset_globals();
  absl::Cleanup cleanup = [] { ncdump_stub_reset_globals(); };

  constexpr int kModes[] = {NC_DISKLESS, NC_NETCDF4 | NC_DISKLESS};
  for (const int mode : kModes) {
    SCOPED_TRACE(mode);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("iter_single.nc", mode, nc.ptr()));

    std::array<int, 2> dimids{};
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "r", 3, &dimids[0]));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "c", 4, &dimids[1]));

    int varid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "v", NC_INT, 2, dimids.data(), &varid));
    if ((mode & NC_NETCDF4) != 0) {
      ASSERT_EQ(NC_NOERR,
                nc_def_var_chunking(nc.get(), varid, NC_COMPACT, nullptr));
    }
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    std::vector<int32_t> expected(12);
    std::iota(expected.begin(), expected.end(), 100);
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), varid, expected.data()));

    NcIterGuard guard;
    ASSERT_EQ(NC_NOERR, nc_get_iter(nc.get(), varid, 64, &guard.iter));
    EXPECT_EQ(0, guard.iter->chunked);

    std::array<size_t, NC_MAX_VAR_DIMS> start{};
    std::array<size_t, NC_MAX_VAR_DIMS> count{};
    const size_t first = nc_next_iter(guard.iter, start.data(), count.data());
    ASSERT_EQ(12UL, first);
    EXPECT_EQ(0UL, start[0]);
    EXPECT_EQ(0UL, start[1]);
    EXPECT_EQ(3UL, count[0]);
    EXPECT_EQ(4UL, count[1]);

    std::vector<int32_t> actual(12, -999);
    ASSERT_NO_FATAL_FAILURE(
        ReadSlab2D(nc.get(), varid, start.data(), count.data(), 4, actual));
    EXPECT_EQ(expected, actual);

    EXPECT_EQ(0UL, nc_next_iter(guard.iter, start.data(), count.data()));
  }
}

TEST(NciterTest, ContiguousMultiDimEvenAndLeftoverSlicesAndOdometerCarry) {
  ncdump_stub_reset_globals();
  absl::Cleanup cleanup = [] { ncdump_stub_reset_globals(); };

  // Subcase A: 2D [4, 5] int32 (row = 20B), bufsize = 40B -> 2 rows x 2 blocks,
  // leftover = 0.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create("iter_even.nc", NC_NETCDF4 | NC_DISKLESS, nc.ptr()));
    std::array<int, 2> dimids{};
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "d0", 4, &dimids[0]));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "d1", 5, &dimids[1]));
    int varid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "v", NC_INT, 2, dimids.data(), &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    std::vector<int32_t> expected(20);
    std::iota(expected.begin(), expected.end(), 1);
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), varid, expected.data()));

    NcIterGuard guard;
    ASSERT_EQ(NC_NOERR, nc_get_iter(nc.get(), varid, 40, &guard.iter));

    std::array<size_t, NC_MAX_VAR_DIMS> start{};
    std::array<size_t, NC_MAX_VAR_DIMS> count{};
    std::vector<Slab> slabs;
    std::vector<int32_t> actual(20, -1);
    size_t to_get = 0;
    while ((to_get = nc_next_iter(guard.iter, start.data(), count.data())) >
           0) {
      slabs.push_back({{start[0], start[1]}, {count[0], count[1]}, to_get});
      ASSERT_NO_FATAL_FAILURE(
          ReadSlab2D(nc.get(), varid, start.data(), count.data(), 5, actual));
    }
    const std::vector<Slab> expected_slabs = {
        {{0, 0}, {2, 5}, 10},
        {{2, 0}, {2, 5}, 10},
    };
    EXPECT_EQ(expected_slabs, slabs);
    EXPECT_EQ(expected, actual);
  }

  // Subcase B: 3D [2, 5, 3] int32 (dim2 = 12B, dim1*dim2 = 60B), bufsize = 24B
  // -> right_dim = 1, rows = 2, numrows = 2, leftover = 1, plus carry across
  // d0.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create("iter_3d.nc", NC_NETCDF4 | NC_DISKLESS, nc.ptr()));
    std::array<int, 3> dimids{};
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "d0", 2, &dimids[0]));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "d1", 5, &dimids[1]));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "d2", 3, &dimids[2]));
    int varid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "v3", NC_INT, 3, dimids.data(), &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    std::vector<int32_t> expected(30);
    std::iota(expected.begin(), expected.end(), 10);
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), varid, expected.data()));

    NcIterGuard guard;
    ASSERT_EQ(NC_NOERR, nc_get_iter(nc.get(), varid, 24, &guard.iter));
    EXPECT_EQ(1, guard.iter->right_dim);

    std::array<size_t, NC_MAX_VAR_DIMS> start{};
    std::array<size_t, NC_MAX_VAR_DIMS> count{};
    std::vector<Slab> slabs;
    std::vector<int32_t> actual(30, -1);
    size_t to_get = 0;
    while ((to_get = nc_next_iter(guard.iter, start.data(), count.data())) >
           0) {
      slabs.push_back({{start[0], start[1], start[2]},
                       {count[0], count[1], count[2]},
                       to_get});
      ASSERT_NO_FATAL_FAILURE(ReadSlab3D(nc.get(), varid, start.data(),
                                         count.data(), 5, 3, actual));
    }
    const std::vector<Slab> expected_slabs = {
        {{0, 0, 0}, {1, 2, 3}, 6}, {{0, 2, 0}, {1, 2, 3}, 6},
        {{0, 4, 0}, {1, 1, 3}, 3}, {{1, 0, 0}, {1, 2, 3}, 6},
        {{1, 2, 0}, {1, 2, 3}, 6}, {{1, 4, 0}, {1, 1, 3}, 3},
    };
    EXPECT_EQ(expected_slabs, slabs);
    EXPECT_EQ(expected, actual);
  }

  // Subcase C: 2D [2, 5] int32 with bufsize = 8B (smaller than a single row),
  // forcing right_dim = 1 (rank - 1) and leftover = 1.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create("iter_subrow.nc", NC_NETCDF4 | NC_DISKLESS, nc.ptr()));
    std::array<int, 2> dimids{};
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "d0", 2, &dimids[0]));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "d1", 5, &dimids[1]));
    int varid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "v_sub", NC_INT, 2, dimids.data(), &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    std::vector<int32_t> expected(10);
    std::iota(expected.begin(), expected.end(), 1);
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), varid, expected.data()));

    NcIterGuard guard;
    ASSERT_EQ(NC_NOERR, nc_get_iter(nc.get(), varid, 8, &guard.iter));
    EXPECT_EQ(1, guard.iter->right_dim);

    std::array<size_t, NC_MAX_VAR_DIMS> start{};
    std::array<size_t, NC_MAX_VAR_DIMS> count{};
    std::vector<int32_t> actual(10, -1);
    size_t total = 0;
    size_t to_get = 0;
    while ((to_get = nc_next_iter(guard.iter, start.data(), count.data())) >
           0) {
      total += to_get;
      ASSERT_NO_FATAL_FAILURE(
          ReadSlab2D(nc.get(), varid, start.data(), count.data(), 5, actual));
    }
    EXPECT_EQ(10UL, total);
    EXPECT_EQ(expected, actual);
  }
}

TEST(NciterTest, ChunkedMultiDimAndUnlimitedClampedChunks) {
  ncdump_stub_reset_globals();
  absl::Cleanup cleanup = [] { ncdump_stub_reset_globals(); };

  // Subcase A: Fixed 2D [5, 3] with chunkshape [2, 2] exercises multi-chunk
  // carries and boundary clamping on both dimensions.
  {
    ScopedMemio memio;
    {
      ScopedNcFile writer;
      ASSERT_EQ(NC_NOERR, nc_create_mem("iter_chunked.nc", NC_NETCDF4, 4096,
                                        writer.ptr()));
      std::array<int, 2> dimids{};
      ASSERT_EQ(NC_NOERR, nc_def_dim(writer.get(), "d0", 5, &dimids[0]));
      ASSERT_EQ(NC_NOERR, nc_def_dim(writer.get(), "d1", 3, &dimids[1]));
      int varid = -1;
      ASSERT_EQ(NC_NOERR, nc_def_var(writer.get(), "vc", NC_INT, 2,
                                     dimids.data(), &varid));
      const std::array<size_t, 2> chunks = {2, 2};
      ASSERT_EQ(NC_NOERR, nc_def_var_chunking(writer.get(), varid, NC_CHUNKED,
                                              chunks.data()));
      ASSERT_EQ(NC_NOERR, nc_enddef(writer.get()));

      std::vector<int32_t> data(15);
      std::iota(data.begin(), data.end(), 1);
      ASSERT_EQ(NC_NOERR, nc_put_var_int(writer.get(), varid, data.data()));
      ASSERT_EQ(NC_NOERR, nc_close_memio(writer.release(), memio.get()));
    }

    ScopedNcFile reader;
    ASSERT_EQ(NC_NOERR, nc_open_mem("iter_chunked.nc", NC_NOWRITE, memio.size(),
                                    memio.get()->memory, reader.ptr()));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(reader.get(), "vc", &varid));

    NcIterGuard guard;
    ASSERT_EQ(NC_NOERR, nc_get_iter(reader.get(), varid, 32, &guard.iter));
    EXPECT_EQ(1, guard.iter->chunked);

    std::array<size_t, NC_MAX_VAR_DIMS> start{};
    std::array<size_t, NC_MAX_VAR_DIMS> count{};
    std::vector<Slab> slabs;
    std::vector<int32_t> actual(15, -1);
    size_t to_get = 0;
    while ((to_get = nc_next_iter(guard.iter, start.data(), count.data())) >
           0) {
      slabs.push_back({{start[0], start[1]}, {count[0], count[1]}, to_get});
      ASSERT_NO_FATAL_FAILURE(ReadSlab2D(reader.get(), varid, start.data(),
                                         count.data(), 3, actual));
    }
    const std::vector<Slab> expected_slabs = {
        {{0, 0}, {2, 2}, 4}, {{0, 2}, {2, 1}, 2}, {{2, 0}, {2, 2}, 4},
        {{2, 2}, {2, 1}, 2}, {{4, 0}, {1, 2}, 2}, {{4, 2}, {1, 1}, 1},
    };
    EXPECT_EQ(expected_slabs, slabs);

    std::vector<int32_t> expected(15);
    std::iota(expected.begin(), expected.end(), 1);
    EXPECT_EQ(expected, actual);
  }

  // Subcase B: Unlimited dimension shorter than its chunksize (dimlen = 2,
  // chunksize = 4) exercises chunk > dimsize clamping on first and later steps.
  {
    ScopedMemio memio;
    {
      ScopedNcFile writer;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("iter_unlim.nc", NC_NETCDF4, 4096, writer.ptr()));
      std::array<int, 2> dimids{};
      ASSERT_EQ(NC_NOERR,
                nc_def_dim(writer.get(), "t", NC_UNLIMITED, &dimids[0]));
      ASSERT_EQ(NC_NOERR, nc_def_dim(writer.get(), "x", 5, &dimids[1]));
      int varid = -1;
      ASSERT_EQ(NC_NOERR, nc_def_var(writer.get(), "vu", NC_INT, 2,
                                     dimids.data(), &varid));
      const std::array<size_t, 2> chunks = {4, 2};
      ASSERT_EQ(NC_NOERR, nc_def_var_chunking(writer.get(), varid, NC_CHUNKED,
                                              chunks.data()));
      ASSERT_EQ(NC_NOERR, nc_enddef(writer.get()));

      std::vector<int32_t> data(10);
      std::iota(data.begin(), data.end(), 50);
      const std::array<size_t, 2> start = {0, 0};
      const std::array<size_t, 2> count = {2, 5};
      ASSERT_EQ(NC_NOERR, nc_put_vara_int(writer.get(), varid, start.data(),
                                          count.data(), data.data()));
      ASSERT_EQ(NC_NOERR, nc_close_memio(writer.release(), memio.get()));
    }

    ScopedNcFile reader;
    ASSERT_EQ(NC_NOERR, nc_open_mem("iter_unlim.nc", NC_NOWRITE, memio.size(),
                                    memio.get()->memory, reader.ptr()));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(reader.get(), "vu", &varid));

    NcIterGuard guard;
    ASSERT_EQ(NC_NOERR, nc_get_iter(reader.get(), varid, 64, &guard.iter));
    EXPECT_EQ(1, guard.iter->chunked);

    std::array<size_t, NC_MAX_VAR_DIMS> start{};
    std::array<size_t, NC_MAX_VAR_DIMS> count{};
    std::vector<Slab> slabs;
    std::vector<int32_t> actual(10, -1);
    size_t to_get = 0;
    while ((to_get = nc_next_iter(guard.iter, start.data(), count.data())) >
           0) {
      slabs.push_back({{start[0], start[1]}, {count[0], count[1]}, to_get});
      ASSERT_NO_FATAL_FAILURE(ReadSlab2D(reader.get(), varid, start.data(),
                                         count.data(), 5, actual));
    }
    const std::vector<Slab> expected_slabs = {
        {{0, 0}, {2, 2}, 4},
        {{0, 2}, {2, 2}, 4},
        {{0, 4}, {2, 1}, 2},
    };
    EXPECT_EQ(expected_slabs, slabs);

    std::vector<int32_t> expected(10);
    std::iota(expected.begin(), expected.end(), 50);
    EXPECT_EQ(expected, actual);
  }
}

TEST(NciterTest, ChunkLargerThanBufferAndInvalidIdsExitInChild) {
  const CliResult chunk_too_big = RunCliInForkedChild(
      [](int, char**) -> int {
        ncdump_stub_reset_globals();
        ScopedNcFile nc;
        if (nc_create("iter_bigchunk.nc", NC_NETCDF4 | NC_DISKLESS, nc.ptr()) !=
            NC_NOERR) {
          return 2;
        }
        std::array<int, 2> dimids{};
        if (nc_def_dim(nc.get(), "a", 4, &dimids[0]) != NC_NOERR ||
            nc_def_dim(nc.get(), "b", 4, &dimids[1]) != NC_NOERR) {
          return 2;
        }
        int varid = -1;
        if (nc_def_var(nc.get(), "big", NC_DOUBLE, 2, dimids.data(), &varid) !=
            NC_NOERR) {
          return 2;
        }
        const std::array<size_t, 2> chunks = {4, 4};
        if (nc_def_var_chunking(nc.get(), varid, NC_CHUNKED, chunks.data()) !=
                NC_NOERR ||
            nc_enddef(nc.get()) != NC_NOERR) {
          return 2;
        }
        nciter_t* iter = nullptr;
        // 4 * 4 * sizeof(double) == 128 > bufsize (64) triggers NC_CHECK
        // exit(1).
        nc_get_iter(nc.get(), varid, 64, &iter);
        return 0;
      },
      kChildArgv);
  ASSERT_TRUE(chunk_too_big.ok()) << chunk_too_big.harness_error;
  EXPECT_EQ(1, chunk_too_big.exit_code);
  EXPECT_THAT(chunk_too_big.stderr_output, ::testing::HasSubstr("chunksize"));

  const CliResult bad_id = RunCliInForkedChild(
      [](int, char**) -> int {
        ncdump_stub_reset_globals();
        nciter_t* iter = nullptr;
        nc_get_iter(-1, 0, 1024, &iter);
        return 0;
      },
      kChildArgv);
  ASSERT_TRUE(bad_id.ok()) << bad_id.harness_error;
  EXPECT_EQ(1, bad_id.exit_code);
  EXPECT_FALSE(bad_id.stderr_output.empty());
}

}  // namespace
}  // namespace netcdf
