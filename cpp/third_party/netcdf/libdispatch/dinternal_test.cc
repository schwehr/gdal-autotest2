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

// Unit tests for //third_party/netcdf/libdispatch/dinternal.c.
//
// Exercises NCDISPATCH_inq_var_all and NCDISPATCH_get_att across invalid/stale
// dataset handles, NetCDF-3 classic/CDF5, and NetCDF-4/HDF5 dispatch paths.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dinternal.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

extern "C" {
#include "third_party/netcdf/include/ncdispatch.h"
}  // extern "C"

namespace netcdf {
namespace {

TEST(DinternalTest, InvalidAndClosedIdsReturnBadId) {
  char name[NC_MAX_NAME + 1]{};
  nc_type xtype = NC_NAT;
  int ndims = -1;
  int natts = -1;
  EXPECT_EQ(NC_EBADID,
            NCDISPATCH_inq_var_all(-1, 0, name, &xtype, &ndims, nullptr, &natts,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr));

  int32_t att_val = -1;
  EXPECT_EQ(NC_EBADID,
            NCDISPATCH_get_att(-1, NC_GLOBAL, "missing", &att_val, NC_INT));

  // Verify stale ID after close returns NC_EBADID rather than succeeding.
  int stale_id = -1;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dinternal_stale.nc", NC_CLOBBER, 0, nc.ptr()));
    stale_id = nc.get();
  }
  EXPECT_EQ(NC_EBADID,
            NCDISPATCH_inq_var_all(stale_id, 0, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_EBADID, NCDISPATCH_get_att(stale_id, NC_GLOBAL, "missing",
                                          &att_val, NC_INT));
}

TEST(DinternalTest, Netcdf3DispatchInqVarAllAndGetAtt) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dinternal_nc3.nc", NC_CLOBBER, 0, nc.ptr()));

  int dim_x = -1;
  int dim_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 3, &dim_x));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "y", 2, &dim_y));
  const std::array<int, 2> dims{dim_x, dim_y};

  int varid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc.get(), "pressure", NC_FLOAT, 2, dims.data(), &varid));
  const float custom_fill = -99.5F;
  ASSERT_EQ(NC_NOERR, nc_put_att_float(nc.get(), varid, NC_FillValue, NC_FLOAT,
                                       1, &custom_fill));
  const std::array<int, 2> scale_vals{10, 25};
  ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), varid, "scale", NC_INT,
                                     scale_vals.size(), scale_vals.data()));
  constexpr char kTitle[] = "nc3_dispatch";
  ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), NC_GLOBAL, "title",
                                      std::strlen(kTitle), kTitle));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

  // Query all supported NC3 fields through NCDISPATCH_inq_var_all.
  char var_name[NC_MAX_NAME + 1]{};
  nc_type xtype = NC_NAT;
  int ndims = -1;
  std::array<int, 2> out_dims{-1, -1};
  int natts = -1;
  int shuffle = -1;
  int deflate = -1;
  int fletcher32 = -1;
  int contiguous = -1;
  int no_fill = -1;
  float fill_val = 0.0F;
  ASSERT_EQ(NC_NOERR, NCDISPATCH_inq_var_all(
                          nc.get(), varid, var_name, &xtype, &ndims,
                          out_dims.data(), &natts, &shuffle, &deflate, nullptr,
                          &fletcher32, &contiguous, nullptr, &no_fill,
                          &fill_val, nullptr, nullptr, nullptr, nullptr));
  EXPECT_STREQ("pressure", var_name);
  EXPECT_EQ(NC_FLOAT, xtype);
  EXPECT_EQ(2, ndims);
  EXPECT_THAT(out_dims, ::testing::ElementsAre(dim_x, dim_y));
  EXPECT_EQ(2, natts);
  EXPECT_EQ(0, shuffle);
  EXPECT_EQ(0, deflate);
  EXPECT_EQ(0, fletcher32);
  EXPECT_EQ(NC_CONTIGUOUS, contiguous);
  EXPECT_EQ(0, no_fill);
  EXPECT_FLOAT_EQ(custom_fill, fill_val);

  // NC3 rejects netCDF-4-specific output pointers and unknown variables.
  int endianness = -1;
  EXPECT_EQ(NC_ENOTNC4,
            NCDISPATCH_inq_var_all(nc.get(), varid, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   &endianness, nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_ENOTVAR,
            NCDISPATCH_inq_var_all(nc.get(), 99, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr));

  // Verify NCDISPATCH_get_att for exact type, NC_NAT, type conversion, and
  // errors.
  std::array<int, 2> read_int{-1, -1};
  ASSERT_EQ(NC_NOERR, NCDISPATCH_get_att(nc.get(), varid, "scale",
                                         read_int.data(), NC_INT));
  EXPECT_THAT(read_int, ::testing::ElementsAre(10, 25));

  std::array<double, 2> read_double{0.0, 0.0};
  ASSERT_EQ(NC_NOERR, NCDISPATCH_get_att(nc.get(), varid, "scale",
                                         read_double.data(), NC_DOUBLE));
  EXPECT_THAT(read_double, ::testing::ElementsAre(10.0, 25.0));

  std::array<char, sizeof(kTitle)> read_text{};
  ASSERT_EQ(NC_NOERR, NCDISPATCH_get_att(nc.get(), NC_GLOBAL, "title",
                                         read_text.data(), NC_NAT));
  EXPECT_STREQ(kTitle, read_text.data());

  int dummy = 0;
  EXPECT_EQ(NC_ENOTATT,
            NCDISPATCH_get_att(nc.get(), varid, "no_such_att", &dummy, NC_INT));
  EXPECT_EQ(NC_ENOTVAR,
            NCDISPATCH_get_att(nc.get(), 42, "scale", &dummy, NC_INT));
  EXPECT_EQ(NC_ECHAR,
            NCDISPATCH_get_att(nc.get(), NC_GLOBAL, "title", &dummy, NC_INT));
}

TEST(DinternalTest, Netcdf4DispatchInqVarAllAndGetAtt) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dinternal_nc4.nc", NC_NETCDF4, 0, nc.ptr()));

  int dim_x = -1;
  int dim_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 4, &dim_x));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "y", 6, &dim_y));
  const std::array<int, 2> dims{dim_x, dim_y};
  const std::array<size_t, 2> chunks{2, 3};

  int varid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc.get(), "temp", NC_DOUBLE, 2, dims.data(), &varid));
  ASSERT_EQ(NC_NOERR,
            nc_def_var_chunking(nc.get(), varid, NC_CHUNKED, chunks.data()));
  ASSERT_EQ(NC_NOERR, nc_def_var_deflate(nc.get(), varid, /*shuffle=*/1,
                                         /*deflate=*/0, /*deflate_level=*/0));
  ASSERT_EQ(NC_NOERR, nc_def_var_fletcher32(nc.get(), varid, NC_FLETCHER32));
  ASSERT_EQ(NC_NOERR, nc_def_var_endian(nc.get(), varid, NC_ENDIAN_LITTLE));

  const double custom_fill = -1234.5;
  ASSERT_EQ(NC_NOERR, nc_def_var_fill(nc.get(), varid, NC_FILL, &custom_fill));

  const char* labels[] = {"kelvin", "celsius"};
  ASSERT_EQ(NC_NOERR, nc_put_att_string(nc.get(), varid, "units", 2, labels));
  const long long rev = 7;
  ASSERT_EQ(NC_NOERR, nc_put_att_longlong(nc.get(), NC_GLOBAL, "revision",
                                          NC_INT64, 1, &rev));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

  // Global attribute count query via NC_GLOBAL.
  int global_natts = -1;
  ASSERT_EQ(NC_NOERR,
            NCDISPATCH_inq_var_all(
                nc.get(), NC_GLOBAL, nullptr, nullptr, nullptr, nullptr,
                &global_natts, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(1, global_natts);

  // Query NetCDF-4 metadata (deflatep and idp must be NULL for
  // NC4_inq_var_all).
  char var_name[NC_MAX_NAME + 1]{};
  nc_type xtype = NC_NAT;
  int ndims = -1;
  std::array<int, 2> out_dims{-1, -1};
  int natts = -1;
  int shuffle = -1;
  int fletcher32 = -1;
  int storage = -1;
  std::array<size_t, 2> out_chunks{};
  int no_fill = -1;
  double fill_val = 0.0;
  int endianness = -1;
  ASSERT_EQ(NC_NOERR, NCDISPATCH_inq_var_all(
                          nc.get(), varid, var_name, &xtype, &ndims,
                          out_dims.data(), &natts, &shuffle, nullptr, nullptr,
                          &fletcher32, &storage, out_chunks.data(), &no_fill,
                          &fill_val, &endianness, nullptr, nullptr, nullptr));
  EXPECT_STREQ("temp", var_name);
  EXPECT_EQ(NC_DOUBLE, xtype);
  EXPECT_EQ(2, ndims);
  EXPECT_THAT(out_dims, ::testing::ElementsAre(dim_x, dim_y));
  EXPECT_EQ(2, natts);
  EXPECT_EQ(1, shuffle);
  EXPECT_EQ(1, fletcher32);
  EXPECT_EQ(NC_CHUNKED, storage);
  EXPECT_THAT(out_chunks, ::testing::ElementsAre(2U, 3U));
  EXPECT_EQ(0, no_fill);
  EXPECT_DOUBLE_EQ(custom_fill, fill_val);
  EXPECT_EQ(NC_ENDIAN_LITTLE, endianness);

  // NC4_inq_var_all returns NC_EFILTER when legacy deflatep or idp is passed.
  int deflate_flag = 0;
  unsigned int filter_id = 0;
  EXPECT_EQ(NC_EFILTER,
            NCDISPATCH_inq_var_all(
                nc.get(), varid, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, &deflate_flag, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_EFILTER,
            NCDISPATCH_inq_var_all(nc.get(), varid, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, &filter_id, nullptr, nullptr));
  EXPECT_EQ(NC_ENOTVAR,
            NCDISPATCH_inq_var_all(nc.get(), 99, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr));

  // Verify NCDISPATCH_get_att for global NC_INT64, var NC_STRING, and errors.
  int64_t read_rev = 0;
  ASSERT_EQ(NC_NOERR, NCDISPATCH_get_att(nc.get(), NC_GLOBAL, "revision",
                                         &read_rev, NC_NAT));
  EXPECT_EQ(rev, read_rev);

  double read_rev_as_double = 0.0;
  ASSERT_EQ(NC_NOERR, NCDISPATCH_get_att(nc.get(), NC_GLOBAL, "revision",
                                         &read_rev_as_double, NC_DOUBLE));
  EXPECT_DOUBLE_EQ(7.0, read_rev_as_double);

  {
    ScopedNcStrings read_units(2);
    ASSERT_EQ(NC_NOERR, NCDISPATCH_get_att(nc.get(), varid, "units",
                                           read_units.data(), NC_STRING));
    EXPECT_STREQ("kelvin", read_units[0]);
    EXPECT_STREQ("celsius", read_units[1]);
  }

  int missing_buf = 0;
  EXPECT_EQ(NC_ENOTATT, NCDISPATCH_get_att(nc.get(), varid, "missing_att",
                                           &missing_buf, NC_INT));
}

}  // namespace
}  // namespace netcdf
