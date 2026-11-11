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

// Unit tests for //third_party/netcdf/libsrc4/nc4cache.c.
//
// Covers process-wide HDF5 chunk cache size/nelems/preemption validation,
// NULL out-pointer handling in both size_t/float and Fortran int wrappers,
// preservation of existing settings on error, and propagation to new netCDF-4
// variables.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libsrc4/nc4cache.c

#include <cstddef>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_f.h"
// clang-format on

namespace netcdf {
namespace {

TEST(Nc4CacheTest, SetAndGetChunkCacheRoundTripsAndAcceptsNullOutputs) {
  ScopedChunkCache cache_guard;

  struct ValidCase {
    const char* label;
    size_t size;
    size_t nelems;
    float preemption;
  };
  constexpr ValidCase kCases[] = {
      {"zero_preemption", 262144, 127, 0.0F},
      {"mid_preemption", 1048576, 521, 0.5F},
      {"full_preemption", 4194304, 2003, 1.0F},
  };

  for (const ValidCase& tc : kCases) {
    SCOPED_TRACE(tc.label);
    ASSERT_EQ(NC_NOERR, nc_set_chunk_cache(tc.size, tc.nelems, tc.preemption));

    size_t got_size = 0;
    size_t got_nelems = 0;
    float got_preemption = -1.0F;
    ASSERT_EQ(NC_NOERR,
              nc_get_chunk_cache(&got_size, &got_nelems, &got_preemption));
    EXPECT_EQ(tc.size, got_size);
    EXPECT_EQ(tc.nelems, got_nelems);
    EXPECT_FLOAT_EQ(tc.preemption, got_preemption);
  }

  // Exercise each optional NULL output pointer branch individually and all
  // NULL together.
  ASSERT_EQ(NC_NOERR, nc_set_chunk_cache(524288, 257, 0.25F));
  EXPECT_EQ(NC_NOERR, nc_get_chunk_cache(nullptr, nullptr, nullptr));

  size_t only_size = 0;
  ASSERT_EQ(NC_NOERR, nc_get_chunk_cache(&only_size, nullptr, nullptr));
  EXPECT_EQ(size_t{524288}, only_size);

  size_t only_nelems = 0;
  ASSERT_EQ(NC_NOERR, nc_get_chunk_cache(nullptr, &only_nelems, nullptr));
  EXPECT_EQ(size_t{257}, only_nelems);

  float only_preemption = -1.0F;
  ASSERT_EQ(NC_NOERR, nc_get_chunk_cache(nullptr, nullptr, &only_preemption));
  EXPECT_FLOAT_EQ(0.25F, only_preemption);
}

TEST(Nc4CacheTest,
     SetChunkCacheRejectsOutOfRangePreemptionWithoutMutatingState) {
  ScopedChunkCache cache_guard;

  constexpr size_t kBaselineSize = 2097152;
  constexpr size_t kBaselineNelems = 1009;
  constexpr float kBaselinePreemption = 0.75F;
  ASSERT_EQ(NC_NOERR, nc_set_chunk_cache(kBaselineSize, kBaselineNelems,
                                         kBaselinePreemption));

  constexpr float kInvalidPreemptions[] = {-0.01F, -1.0F, 1.01F, 2.0F};
  for (float bad_preemption : kInvalidPreemptions) {
    SCOPED_TRACE(bad_preemption);
    EXPECT_EQ(NC_EINVAL, nc_set_chunk_cache(65536, 31, bad_preemption));

    size_t got_size = 0;
    size_t got_nelems = 0;
    float got_preemption = -1.0F;
    ASSERT_EQ(NC_NOERR,
              nc_get_chunk_cache(&got_size, &got_nelems, &got_preemption));
    EXPECT_EQ(kBaselineSize, got_size);
    EXPECT_EQ(kBaselineNelems, got_nelems);
    EXPECT_FLOAT_EQ(kBaselinePreemption, got_preemption);
  }
}

TEST(Nc4CacheTest, SetAndGetChunkCacheIntsRoundTripsAndAcceptsNullOutputs) {
  ScopedChunkCache cache_guard;

  // Use exact binary-fraction percentages (0, 25, 50, 75, 100) so float
  // conversion `(float)preemption / 100` * 100 truncates back cleanly.
  struct IntCase {
    const char* label;
    int size;
    int nelems;
    int preemption;
  };
  constexpr IntCase kCases[] = {
      {"zero_percent", 131072, 61, 0},
      {"quarter_percent", 262144, 127, 25},
      {"half_percent", 524288, 257, 50},
      {"three_quarter_percent", 1048576, 521, 75},
      {"full_percent", 2097152, 1009, 100},
  };

  for (const IntCase& tc : kCases) {
    SCOPED_TRACE(tc.label);
    ASSERT_EQ(NC_NOERR,
              nc_set_chunk_cache_ints(tc.size, tc.nelems, tc.preemption));

    int got_size = -1;
    int got_nelems = -1;
    int got_preemption = -1;
    ASSERT_EQ(NC_NOERR,
              nc_get_chunk_cache_ints(&got_size, &got_nelems, &got_preemption));
    EXPECT_EQ(tc.size, got_size);
    EXPECT_EQ(tc.nelems, got_nelems);
    EXPECT_EQ(tc.preemption, got_preemption);
  }

  ASSERT_EQ(NC_NOERR, nc_set_chunk_cache_ints(786432, 389, 50));
  EXPECT_EQ(NC_NOERR, nc_get_chunk_cache_ints(nullptr, nullptr, nullptr));

  int only_size = -1;
  ASSERT_EQ(NC_NOERR, nc_get_chunk_cache_ints(&only_size, nullptr, nullptr));
  EXPECT_EQ(786432, only_size);

  int only_nelems = -1;
  ASSERT_EQ(NC_NOERR, nc_get_chunk_cache_ints(nullptr, &only_nelems, nullptr));
  EXPECT_EQ(389, only_nelems);

  int only_preemption = -1;
  ASSERT_EQ(NC_NOERR,
            nc_get_chunk_cache_ints(nullptr, nullptr, &only_preemption));
  EXPECT_EQ(50, only_preemption);
}

TEST(Nc4CacheTest,
     SetChunkCacheIntsRejectsInvalidArgumentsWithoutMutatingState) {
  ScopedChunkCache cache_guard;

  constexpr int kBaselineSize = 1048576;
  constexpr int kBaselineNelems = 521;
  constexpr int kBaselinePreemption = 75;
  ASSERT_EQ(NC_NOERR, nc_set_chunk_cache_ints(kBaselineSize, kBaselineNelems,
                                              kBaselinePreemption));

  struct InvalidCase {
    const char* label;
    int size;
    int nelems;
    int preemption;
  };
  constexpr InvalidCase kInvalidCases[] = {
      {"zero_size", 0, 100, 50},
      {"negative_size", -1, 100, 50},
      {"zero_nelems", 4096, 0, 50},
      {"negative_nelems", 4096, -5, 50},
      {"negative_preemption", 4096, 100, -1},
      {"over_100_preemption", 4096, 100, 101},
  };

  for (const InvalidCase& tc : kInvalidCases) {
    SCOPED_TRACE(tc.label);
    EXPECT_EQ(NC_EINVAL,
              nc_set_chunk_cache_ints(tc.size, tc.nelems, tc.preemption));

    int got_size = -1;
    int got_nelems = -1;
    int got_preemption = -1;
    ASSERT_EQ(NC_NOERR,
              nc_get_chunk_cache_ints(&got_size, &got_nelems, &got_preemption));
    EXPECT_EQ(kBaselineSize, got_size);
    EXPECT_EQ(kBaselineNelems, got_nelems);
    EXPECT_EQ(kBaselinePreemption, got_preemption);
  }
}

TEST(Nc4CacheTest, NewlyDefinedVariableInheritsUpdatedGlobalChunkCache) {
  ScopedChunkCache cache_guard;

  constexpr size_t kCustomSize = 2097152;
  constexpr size_t kCustomNelems = 1009;
  constexpr float kCustomPreemption = 0.25F;
  ASSERT_EQ(NC_NOERR,
            nc_set_chunk_cache(kCustomSize, kCustomNelems, kCustomPreemption));

  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create("nc4cache_inherit.nc", NC_NETCDF4 | NC_DISKLESS,
                                file.ptr()));
  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "x", 8, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "data", NC_INT, 1, &dimid, &varid));

  size_t var_size = 0;
  size_t var_nelems = 0;
  float var_preemption = -1.0F;
  ASSERT_EQ(NC_NOERR, nc_get_var_chunk_cache(file, varid, &var_size,
                                             &var_nelems, &var_preemption));
  EXPECT_EQ(kCustomSize, var_size);
  EXPECT_EQ(kCustomNelems, var_nelems);
  EXPECT_FLOAT_EQ(kCustomPreemption, var_preemption);
}

}  // namespace
}  // namespace netcdf
