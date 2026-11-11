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

// Unit tests associated with //third_party/netcdf/libsrc4/nc4filters.c.
//
// libsrc4/nc4filters.c contains only comments in NetCDF 4.10.1 (filter helpers
// live in libdispatch/dfilter.c and libhdf5/hdf5filter.c); these smoke tests
// verify built-in netCDF-4 filter availability and in-memory round-trips.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libsrc4/nc4filters.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_filter.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::ElementsAreArray;

TEST(Nc4FiltersTest, ReportsBuiltInAndUnavailableFilters) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("nc4filters_avail.nc", NC_NETCDF4, 0, file.ptr()));

  EXPECT_EQ(NC_NOERR, nc_inq_filter_avail(file, H5Z_FILTER_DEFLATE));
  EXPECT_EQ(NC_NOERR, nc_inq_filter_avail(file, H5Z_FILTER_SHUFFLE));
  EXPECT_EQ(NC_NOERR, nc_inq_filter_avail(file, H5Z_FILTER_FLETCHER32));
  EXPECT_EQ(NC_NOERR, nc_inq_filter_avail(file, H5Z_FILTER_SZIP));
  EXPECT_EQ(NC_ENOFILTER, nc_inq_filter_avail(file, H5Z_FILTER_ZSTD));
}

TEST(Nc4FiltersTest, DeflateAndShuffleChunkedVariableRoundTripsInMemory) {
  constexpr size_t kLen = 16;
  constexpr std::array<int32_t, kLen> kExpected = {
      10, -20,  30,  -40,  50,  -60,  70,  -80,
      90, -100, 110, -120, 130, -140, 150, -160};

  ScopedMemio memio;
  {
    ScopedNcFile writer;
    ASSERT_EQ(NC_NOERR, nc_create_mem("nc4filters_roundtrip.nc", NC_NETCDF4, 0,
                                      writer.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(writer, "x", kLen, &dimid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(writer, "samples", NC_INT, 1, &dimid, &varid));
    constexpr size_t kChunkSize = 8;
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(writer, varid, NC_CHUNKED, &kChunkSize));
    ASSERT_EQ(NC_NOERR, nc_def_var_deflate(writer, varid, NC_SHUFFLE, 1, 5));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(writer, varid, kExpected.data()));
    ASSERT_EQ(NC_NOERR, nc_close_memio(writer.release(), memio.get()));
  }

  ScopedNcFile reader;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("nc4filters_roundtrip.nc", NC_NOWRITE, memio.size(),
                        memio.get()->memory, reader.ptr()));
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(reader, "samples", &varid));

  int shuffle = -1;
  int deflate = -1;
  int deflate_level = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_deflate(reader, varid, &shuffle, &deflate,
                                         &deflate_level));
  EXPECT_EQ(NC_SHUFFLE, shuffle);
  EXPECT_EQ(1, deflate);
  EXPECT_EQ(5, deflate_level);

  std::vector<int32_t> actual(kLen, -999);
  ASSERT_EQ(NC_NOERR, nc_get_var_int(reader, varid, actual.data()));
  EXPECT_THAT(actual, ElementsAreArray(kExpected));
}

}  // namespace
}  // namespace netcdf
