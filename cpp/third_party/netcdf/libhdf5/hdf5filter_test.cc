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

// Clean-room unit tests for //third_party/netcdf/libhdf5/hdf5filter.c.
//
// Exercises public NetCDF-4 filter APIs (avail, def, inq_ids, inq_info, szip,
// deflate) and internal filter-list management helpers (lookup, addfilter,
// ordering of fletcher32/shuffle, remove, freelist, find_missing_filter, and
// default chunksize recomputation).

#include <cstddef>
#include <numeric>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

extern "C" {
#include "third_party/netcdf/include/hdf5internal.h"
}  // extern "C"

namespace netcdf {
namespace {

using ::testing::ElementsAre;

TEST(Hdf5FilterTest, FilterAvailAndInitFinalizeBehaveAsExpected) {
  EXPECT_EQ(NC_NOERR, NC4_hdf5_filter_initialize());
  EXPECT_EQ(NC_NOERR, NC4_hdf5_filter_finalize());
  EXPECT_EQ(NC_NOERR, NC4_hdf5_filter_initialize());

  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("filter_avail.nc", NC_NETCDF4, 4096, file.ptr()));

  // Available built-in HDF5 filters compiled into netCDF/HDF5.
  EXPECT_EQ(NC_NOERR, nc_inq_filter_avail(file, H5Z_FILTER_DEFLATE));
  EXPECT_EQ(NC_NOERR, nc_inq_filter_avail(file, H5Z_FILTER_SHUFFLE));
  EXPECT_EQ(NC_NOERR, nc_inq_filter_avail(file, H5Z_FILTER_FLETCHER32));
  EXPECT_EQ(NC_NOERR, nc_inq_filter_avail(file, H5Z_FILTER_SZIP));

  // Unavailable / unregistered filter IDs within HDF5's valid [0, 65535]
  // range (H5Z_FILTER_MAX) fail with NC_ENOFILTER.
  EXPECT_EQ(NC_ENOFILTER, nc_inq_filter_avail(file, H5Z_FILTER_BZIP2));
  EXPECT_EQ(NC_ENOFILTER, nc_inq_filter_avail(file, H5Z_FILTER_ZSTD));
  EXPECT_EQ(NC_ENOFILTER, nc_inq_filter_avail(file, H5Z_FILTER_BLOSC));
  EXPECT_EQ(NC_ENOFILTER, nc_inq_filter_avail(file, 65535u));

  // Filter IDs exceeding H5Z_FILTER_MAX (65535) cause H5Zfilter_avail() to
  // return FAIL (< 0), which NC4_hdf5_inq_filter_avail() maps to NC_EHDFERR.
  EXPECT_EQ(NC_EHDFERR, nc_inq_filter_avail(file, 65536u));
  EXPECT_EQ(NC_EHDFERR, nc_inq_filter_avail(file, 99999u));

  // Dispatch-level vs internal direct NC4_hdf5_inq_filter_avail check.
  EXPECT_EQ(NC_EBADID, nc_inq_filter_avail(-1, H5Z_FILTER_DEFLATE));
  EXPECT_EQ(NC_NOERR, NC4_hdf5_inq_filter_avail(-1, H5Z_FILTER_DEFLATE));
}

TEST(Hdf5FilterTest, DefVarFilterValidatesIdsModeRankAvailabilityAndParams) {
  const unsigned int kLevel4 = 4;
  // Direct internal call hits NC_check_id failure inside hdf5filter.c.
  EXPECT_EQ(NC_EBADID,
            NC4_hdf5_def_var_filter(-1, 0, H5Z_FILTER_DEFLATE, 1, &kLevel4));

  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("filter_errors.nc", NC_NETCDF4, 4096, file.ptr()));

  int dim_small = -1;
  int dim_wide = -1;
  int dim_unlim = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "small", 4, &dim_small));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "wide", 64, &dim_wide));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "unlim", NC_UNLIMITED, &dim_unlim));

  int scalar_var = -1;
  int small_var = -1;
  int unlim_var = -1;
  int deflate_var = -1;
  int szip_var = -1;
  int created_var = -1;
  const int unlim_dims[2] = {dim_unlim, dim_small};

  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "scalar", NC_INT, 0, nullptr, &scalar_var));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "small_v", NC_INT, 1, &dim_small, &small_var));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "unlim_v", NC_INT, 2, unlim_dims, &unlim_var));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "deflate_v", NC_INT, 1, &dim_wide, &deflate_var));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "szip_v", NC_INT, 1, &dim_wide, &szip_var));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "created_v", NC_INT, 1, &dim_wide, &created_var));

  // Bad group ID bits and nonexistent varid inside NC4_hdf5_def_var_filter.
  EXPECT_EQ(NC_EBADID, nc_def_var_filter(file | 0x7fff, deflate_var,
                                         H5Z_FILTER_DEFLATE, 1, &kLevel4));
  EXPECT_EQ(NC_ENOTVAR,
            nc_def_var_filter(file, 999, H5Z_FILTER_DEFLATE, 1, &kLevel4));

  // Scalars cannot use filters.
  EXPECT_EQ(NC_EINVAL, nc_def_var_filter(file, scalar_var, H5Z_FILTER_DEFLATE,
                                         1, &kLevel4));

  // Unavailable filter IDs within [0, H5Z_FILTER_MAX] fail with NC_ENOFILTER,
  // while IDs above H5Z_FILTER_MAX (65535) fail inside H5Zfilter_avail() and
  // map to NC_EHDFERR.
  EXPECT_EQ(NC_ENOFILTER,
            nc_def_var_filter(file, deflate_var, H5Z_FILTER_ZSTD, 1, &kLevel4));
  EXPECT_EQ(NC_ENOFILTER,
            nc_def_var_filter(file, deflate_var, 65535u, 1, &kLevel4));
  EXPECT_EQ(NC_EHDFERR,
            nc_def_var_filter(file, deflate_var, 65536u, 1, &kLevel4));
  EXPECT_EQ(NC_EHDFERR,
            nc_def_var_filter(file, deflate_var, 99999u, 1, &kLevel4));

  // Deflate parameter validation.
  EXPECT_EQ(NC_EFILTER, nc_def_var_filter(file, deflate_var, H5Z_FILTER_DEFLATE,
                                          0, nullptr));
  const unsigned int kTwoParams[2] = {4, 5};
  EXPECT_EQ(NC_EFILTER, nc_def_var_filter(file, deflate_var, H5Z_FILTER_DEFLATE,
                                          2, kTwoParams));
  const unsigned int kLevelTooHigh = NC_MAX_DEFLATE_LEVEL + 1;
  EXPECT_EQ(NC_EINVAL, nc_def_var_filter(file, deflate_var, H5Z_FILTER_DEFLATE,
                                         1, &kLevelTooHigh));
  const unsigned int kNegativeLevel = static_cast<unsigned int>(-1);
  EXPECT_EQ(NC_EINVAL, nc_def_var_filter(file, deflate_var, H5Z_FILTER_DEFLATE,
                                         1, &kNegativeLevel));

  // Valid deflate at level 0, then overwrite at level 4 (oldspec != nullptr),
  // and verify conflict with szip.
  const unsigned int kLevel0 = 0;
  ASSERT_EQ(NC_NOERR, nc_def_var_filter(file, deflate_var, H5Z_FILTER_DEFLATE,
                                        1, &kLevel0));
  ASSERT_EQ(NC_NOERR, nc_def_var_filter(file, deflate_var, H5Z_FILTER_DEFLATE,
                                        1, &kLevel4));
  const unsigned int kValidSzip[2] = {NC_SZIP_NN, 4};
  EXPECT_EQ(NC_EINVAL, nc_def_var_filter(file, deflate_var, H5Z_FILTER_SZIP, 2,
                                         kValidSzip));

  // Szip parameter validation.
  EXPECT_EQ(NC_EFILTER,
            nc_def_var_filter(file, szip_var, H5Z_FILTER_SZIP, 1, &kLevel4));
  const unsigned int kOddSzip[2] = {NC_SZIP_NN, 3};
  EXPECT_EQ(NC_EINVAL,
            nc_def_var_filter(file, szip_var, H5Z_FILTER_SZIP, 2, kOddSzip));
  const unsigned int kTooManyPixels[2] = {NC_SZIP_NN,
                                          NC_MAX_PIXELS_PER_BLOCK + 2};
  EXPECT_EQ(NC_EINVAL, nc_def_var_filter(file, szip_var, H5Z_FILTER_SZIP, 2,
                                         kTooManyPixels));

  // Pixels-per-block exceeding elements across fixed dims (and skipping
  // unlimited dim length 0) must return NC_EINVAL.
  const unsigned int kSzip8[2] = {NC_SZIP_EC, 8};
  EXPECT_EQ(NC_EINVAL,
            nc_def_var_filter(file, unlim_var, H5Z_FILTER_SZIP, 2, kSzip8));
  // Valid szip <= num_elem (4 <= 4) succeeds, then updating with > num_elem
  // fails even when oldspec != nullptr, while deflate conflicts with szip.
  const unsigned int kSzip4[2] = {NC_SZIP_EC, 4};
  ASSERT_EQ(NC_NOERR,
            nc_def_var_filter(file, small_var, H5Z_FILTER_SZIP, 2, kSzip4));
  EXPECT_EQ(NC_EINVAL,
            nc_def_var_filter(file, small_var, H5Z_FILTER_SZIP, 2, kSzip8));
  EXPECT_EQ(NC_EINVAL, nc_def_var_filter(file, small_var, H5Z_FILTER_DEFLATE, 1,
                                         &kLevel4));

  // Exit define mode: def_var_filter must fail with NC_EINDEFINE.
  ASSERT_EQ(NC_NOERR, nc_enddef(file));
  EXPECT_EQ(NC_EINDEFINE, nc_def_var_filter(file, created_var,
                                            H5Z_FILTER_DEFLATE, 1, &kLevel4));

  // Reenter define mode: already-created HDF5 dataset returns NC_ELATEDEF.
  ASSERT_EQ(NC_NOERR, nc_redef(file));
  EXPECT_EQ(NC_ELATEDEF, nc_def_var_filter(file, created_var,
                                           H5Z_FILTER_DEFLATE, 1, &kLevel4));
}

TEST(Hdf5FilterTest,
     FilterOrderingAndRoundTripPersistenceWorkForDeflateAndSzip) {
  ScopedMemio memio;
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("filter_order.nc", NC_NETCDF4, 8192, file.ptr()));
    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "n", 64, &dimid));

    int v_none = -1;
    int v_df = -1;
    int v_ds = -1;
    int v_fs = -1;
    int v_fd = -1;
    int v_sfd = -1;
    int v_szip = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "none", NC_INT, 1, &dimid, &v_none));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "df", NC_INT, 1, &dimid, &v_df));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "ds", NC_INT, 1, &dimid, &v_ds));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "fs", NC_INT, 1, &dimid, &v_fs));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "fd", NC_INT, 1, &dimid, &v_fd));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "sfd", NC_INT, 1, &dimid, &v_sfd));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "szip", NC_INT, 1, &dimid, &v_szip));

    const unsigned int kLevel5 = 5;
    // 1. deflate then fletcher32 -> [FLETCHER32, DEFLATE].
    ASSERT_EQ(NC_NOERR,
              nc_def_var_filter(file, v_df, H5Z_FILTER_DEFLATE, 1, &kLevel5));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_filter(file, v_df, H5Z_FILTER_FLETCHER32, 0, nullptr));

    // 2. deflate then shuffle -> [SHUFFLE, DEFLATE].
    ASSERT_EQ(NC_NOERR,
              nc_def_var_filter(file, v_ds, H5Z_FILTER_DEFLATE, 1, &kLevel5));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_filter(file, v_ds, H5Z_FILTER_SHUFFLE, 0, nullptr));

    // 3. fletcher32 then shuffle -> [FLETCHER32, SHUFFLE].
    ASSERT_EQ(NC_NOERR,
              nc_def_var_filter(file, v_fs, H5Z_FILTER_FLETCHER32, 0, nullptr));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_filter(file, v_fs, H5Z_FILTER_SHUFFLE, 0, nullptr));

    // 4. fletcher32 then deflate -> [FLETCHER32, DEFLATE].
    ASSERT_EQ(NC_NOERR,
              nc_def_var_filter(file, v_fd, H5Z_FILTER_FLETCHER32, 0, nullptr));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_filter(file, v_fd, H5Z_FILTER_DEFLATE, 1, &kLevel5));

    // 5. shuffle, fletcher32, deflate -> [FLETCHER32, SHUFFLE, DEFLATE].
    ASSERT_EQ(NC_NOERR,
              nc_def_var_filter(file, v_sfd, H5Z_FILTER_SHUFFLE, 0, nullptr));
    ASSERT_EQ(NC_NOERR, nc_def_var_filter(file, v_sfd, H5Z_FILTER_FLETCHER32, 0,
                                          nullptr));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_filter(file, v_sfd, H5Z_FILTER_DEFLATE, 1, &kLevel5));

    // Szip: define with EC then update to NN, 8 pixels/block (oldspec != NULL).
    const unsigned int kSzipEc4[2] = {NC_SZIP_EC, 4};
    const unsigned int kSzipNn8[2] = {NC_SZIP_NN, 8};
    ASSERT_EQ(NC_NOERR,
              nc_def_var_filter(file, v_szip, H5Z_FILTER_SZIP, 2, kSzipEc4));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_filter(file, v_szip, H5Z_FILTER_SZIP, 2, kSzipNn8));

    // Inquiry checks before enddef (count-only, ids-only, both, zero-filter).
    size_t nfilters = 99;
    unsigned int scratch_ids[4] = {};
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_filter_ids(file, v_none, &nfilters, scratch_ids));
    EXPECT_EQ(nfilters, 0u);
    EXPECT_EQ(NC_NOERR, nc_inq_var_filter_ids(file, v_none, nullptr, nullptr));

    size_t sfd_count = 0;
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_filter_ids(file, v_sfd, &sfd_count, nullptr));
    ASSERT_EQ(sfd_count, 3u);
    std::vector<unsigned int> sfd_ids(sfd_count, 0);
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_filter_ids(file, v_sfd, nullptr, sfd_ids.data()));
    EXPECT_THAT(sfd_ids, ElementsAre(H5Z_FILTER_FLETCHER32, H5Z_FILTER_SHUFFLE,
                                     H5Z_FILTER_DEFLATE));

    auto expect_ids = [&](int vid, std::vector<unsigned int> expected) {
      size_t count = 0;
      ASSERT_EQ(NC_NOERR, nc_inq_var_filter_ids(file, vid, &count, nullptr));
      std::vector<unsigned int> actual(count, 0);
      ASSERT_EQ(NC_NOERR,
                nc_inq_var_filter_ids(file, vid, &count, actual.data()));
      EXPECT_EQ(actual, expected);
    };
    expect_ids(v_df, {H5Z_FILTER_FLETCHER32, H5Z_FILTER_DEFLATE});
    expect_ids(v_ds, {H5Z_FILTER_SHUFFLE, H5Z_FILTER_DEFLATE});
    expect_ids(v_fs, {H5Z_FILTER_FLETCHER32, H5Z_FILTER_SHUFFLE});
    expect_ids(v_fd, {H5Z_FILTER_FLETCHER32, H5Z_FILTER_DEFLATE});
    expect_ids(v_szip, {H5Z_FILTER_SZIP});

    // Inq_var_filter_info combinations: absent, zero-param, param with nulls.
    EXPECT_EQ(NC_ENOFILTER,
              nc_inq_var_filter_info(file, v_none, H5Z_FILTER_DEFLATE, nullptr,
                                     nullptr));
    size_t nparams = 99;
    unsigned int param_buf[4] = {};
    ASSERT_EQ(NC_NOERR, nc_inq_var_filter_info(file, v_sfd, H5Z_FILTER_SHUFFLE,
                                               &nparams, param_buf));
    EXPECT_EQ(nparams, 0u);
    ASSERT_EQ(NC_NOERR, nc_inq_var_filter_info(file, v_sfd, H5Z_FILTER_DEFLATE,
                                               nullptr, param_buf));
    EXPECT_EQ(param_buf[0], 5u);
    EXPECT_EQ(NC_NOERR, nc_inq_var_filter_info(file, v_sfd, H5Z_FILTER_DEFLATE,
                                               &nparams, nullptr));
    EXPECT_EQ(nparams, 1u);

    // Direct error checks on inq_var_filter_ids and inq_var_filter_info.
    EXPECT_EQ(NC_EBADID,
              NC4_hdf5_inq_var_filter_ids(-1, v_sfd, &nfilters, scratch_ids));
    EXPECT_EQ(NC_EBADID, nc_inq_var_filter_ids(file | 0x7fff, v_sfd, &nfilters,
                                               scratch_ids));
    EXPECT_EQ(NC_ENOTVAR,
              nc_inq_var_filter_ids(file, 999, &nfilters, scratch_ids));
    EXPECT_EQ(NC_EBADID,
              NC4_hdf5_inq_var_filter_info(-1, v_sfd, H5Z_FILTER_DEFLATE,
                                           &nparams, param_buf));
    EXPECT_EQ(NC_EBADID,
              nc_inq_var_filter_info(file | 0x7fff, v_sfd, H5Z_FILTER_DEFLATE,
                                     &nparams, param_buf));
    EXPECT_EQ(NC_ENOTVAR, nc_inq_var_filter_info(file, 999, H5Z_FILTER_DEFLATE,
                                                 &nparams, param_buf));

    std::vector<int> data(64);
    std::iota(data.begin(), data.end(), 100);
    ASSERT_EQ(NC_NOERR, nc_enddef(file));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(file, v_sfd, data.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(file, v_szip, data.data()));
    ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), memio.get()));
  }

  // Reopen and verify compressed data + filter metadata round-trip cleanly.
  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("filter_order.nc", NC_NOWRITE, memio.size(),
                        const_cast<unsigned char*>(memio.bytes()), ro.ptr()));

  int v_sfd = -1;
  int v_szip = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "sfd", &v_sfd));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "szip", &v_szip));

  size_t sfd_count = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_var_filter_ids(ro, v_sfd, &sfd_count, nullptr));
  ASSERT_EQ(sfd_count, 3u);
  unsigned int sfd_ids[3] = {};
  ASSERT_EQ(NC_NOERR, nc_inq_var_filter_ids(ro, v_sfd, &sfd_count, sfd_ids));
  EXPECT_THAT(sfd_ids, ElementsAre(H5Z_FILTER_FLETCHER32, H5Z_FILTER_SHUFFLE,
                                   H5Z_FILTER_DEFLATE));

  size_t szip_nparams = 0;
  unsigned int szip_params[2] = {};
  ASSERT_EQ(NC_NOERR, nc_inq_var_filter_info(ro, v_szip, H5Z_FILTER_SZIP,
                                             &szip_nparams, szip_params));
  EXPECT_EQ(szip_nparams, 2u);
  EXPECT_EQ(szip_params[0] & NC_SZIP_NN, static_cast<unsigned int>(NC_SZIP_NN));
  EXPECT_EQ(szip_params[1], 8u);

  std::vector<int> expected(64);
  std::iota(expected.begin(), expected.end(), 100);
  std::vector<int> actual_sfd(64, 0);
  std::vector<int> actual_szip(64, 0);
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, v_sfd, actual_sfd.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, v_szip, actual_szip.data()));
  EXPECT_EQ(actual_sfd, expected);
  EXPECT_EQ(actual_szip, expected);
}

TEST(Hdf5FilterTest, InternalFilterListHelpersMissingFilterAndZeroChunksize) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("filter_internal.nc", NC_NETCDF4, 4096, file.ptr()));
  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "x", 16, &dimid));
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v", NC_INT, 1, &dimid, &varid));

  NC_FILE_INFO_T* h5 = nullptr;
  NC_GRP_INFO_T* grp = nullptr;
  NC_VAR_INFO_T* var = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_hdf5_find_grp_h5_var(file, varid, &h5, &grp, &var));
  ASSERT_NE(var, nullptr);
  ASSERT_NE(var->chunksizes, nullptr);

  // Trigger the var->chunksizes && !var->chunksizes[0] branch in
  // NC4_hdf5_def_var_filter to recompute defaults and adjust cache.
  var->chunksizes[0] = 0;
  const unsigned int kLevel2 = 2;
  ASSERT_EQ(NC_NOERR,
            nc_def_var_filter(file, varid, H5Z_FILTER_DEFLATE, 1, &kLevel2));
  EXPECT_GT(var->chunksizes[0], 0u);

  // Also add shuffle (nparams == 0) so freelist frees both nparams>0 & ==0.
  ASSERT_EQ(NC_NOERR,
            nc_def_var_filter(file, varid, H5Z_FILTER_SHUFFLE, 0, nullptr));

  // NC4_hdf5_addfilter rejects nparams > 0 with params == NULL.
  EXPECT_EQ(NC_EINVAL,
            NC4_hdf5_addfilter(var, H5Z_FILTER_DEFLATE, 1, nullptr, 0));

  // Free entire list -> sets var->filters = NULL; second call hits NULL guard.
  EXPECT_EQ(NC_NOERR, NC4_hdf5_filter_freelist(var));
  EXPECT_EQ(var->filters, nullptr);
  EXPECT_EQ(NC_NOERR, NC4_hdf5_filter_freelist(var));

  // Lookup while var->filters == NULL lazily allocates NClist and returns
  // NC_ENOFILTER. Also ensures var->filters != NULL before direct addfilter.
  struct NC_HDF5_Filter* spec = nullptr;
  EXPECT_EQ(NC_ENOFILTER,
            NC4_hdf5_filter_lookup(var, H5Z_FILTER_DEFLATE, &spec));
  EXPECT_NE(var->filters, nullptr);

  // Removing from empty list returns NC_ENOFILTER.
  EXPECT_EQ(NC_ENOFILTER, NC4_hdf5_filter_remove(var, H5Z_FILTER_DEFLATE));

  // Add and overwrite a parameterized filter, add missing flag & shuffle,
  // inspect NC4_hdf5_find_missing_filter, then remove each filter cleanly.
  unsigned int missing_id = 12345u;
  EXPECT_EQ(NC_NOERR, NC4_hdf5_find_missing_filter(var, &missing_id));
  EXPECT_EQ(missing_id, 0u);

  const unsigned int kLevel1 = 1;
  const unsigned int kLevel7 = 7;
  ASSERT_EQ(NC_NOERR,
            NC4_hdf5_addfilter(var, H5Z_FILTER_DEFLATE, 1, &kLevel1, 0));
  ASSERT_EQ(NC_NOERR,
            NC4_hdf5_addfilter(var, H5Z_FILTER_DEFLATE, 1, &kLevel7, 0));
  ASSERT_EQ(NC_NOERR,
            NC4_hdf5_addfilter(var, H5Z_FILTER_SHUFFLE, 0, nullptr, 0));
  constexpr unsigned int kSyntheticMissingId = 99998u;
  ASSERT_EQ(NC_NOERR, NC4_hdf5_addfilter(var, kSyntheticMissingId, 0, nullptr,
                                         NC_HDF5_FILTER_MISSING));

  // Guard cleanup ensures synthetic missing filter is stripped before file
  // destruction even if an assertion fails below.
  absl::Cleanup strip_missing = [&]() {
    NC4_hdf5_filter_remove(var, kSyntheticMissingId);
  };

  EXPECT_EQ(NC_NOERR, NC4_hdf5_find_missing_filter(var, &missing_id));
  EXPECT_EQ(missing_id, kSyntheticMissingId);
  EXPECT_EQ(NC_NOERR, NC4_hdf5_find_missing_filter(var, nullptr));

  // Remove synthetic missing, deflate (nparams > 0), shuffle (nparams == 0).
  std::move(strip_missing).Invoke();
  EXPECT_EQ(NC_NOERR, NC4_hdf5_find_missing_filter(var, &missing_id));
  EXPECT_EQ(missing_id, 0u);

  EXPECT_EQ(NC_NOERR, NC4_hdf5_filter_remove(var, H5Z_FILTER_DEFLATE));
  EXPECT_EQ(NC_NOERR, NC4_hdf5_filter_remove(var, H5Z_FILTER_SHUFFLE));
  EXPECT_EQ(NC_ENOFILTER, NC4_hdf5_filter_remove(var, H5Z_FILTER_SHUFFLE));

  ASSERT_EQ(NC_NOERR, nc_enddef(file));
}

}  // namespace
}  // namespace netcdf
