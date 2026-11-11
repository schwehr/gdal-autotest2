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

// Unit tests for //third_party/netcdf/libdispatch/dfilter.c.
//
// Covers per-variable filter definition/inquiry APIs across NetCDF-4 and
// classic formats, compile-time disabled zstandard/blosc no-ops, unavailable
// filter handling, and standardized bzip2 filter validation via an isolated
// user-defined dispatch table.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dfilter.c

#include <array>
#include <cstddef>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

extern "C" {
#include "third_party/netcdf/include/ncdispatch.h"
}  // extern "C"

namespace netcdf {
namespace {

struct MockBzip2State {
  int avail_status = NC_NOERR;
  int def_status = NC_NOERR;
  int first_inq_status = NC_NOERR;
  int second_inq_status = NC_NOERR;
  int inq_ids_first_status = NC_NOERR;
  int inq_ids_second_status = NC_NOERR;
  bool has_bzip2 = false;
  size_t reported_nparams = 1;
  unsigned int level = 0;
  int inq_calls = 0;
  int reset() {
    avail_status = NC_NOERR;
    def_status = NC_NOERR;
    first_inq_status = NC_NOERR;
    second_inq_status = NC_NOERR;
    inq_ids_first_status = NC_NOERR;
    inq_ids_second_status = NC_NOERR;
    has_bzip2 = false;
    reported_nparams = 1;
    level = 0;
    inq_calls = 0;
    return NC_NOERR;
  }
};

MockBzip2State& GetMockBzip2State() {
  static MockBzip2State state;
  return state;
}

extern "C" {
static int MockUdfCreate(const char* /*path*/, int /*cmode*/,
                         size_t /*initialsz*/, int /*basepe*/,
                         size_t* /*chunksizehintp*/, void* /*parameters*/,
                         const struct NC_Dispatch* /*table*/, int /*ncid*/) {
  return NC_NOERR;
}

static int MockUdfClose(int /*ncid*/, void* /*memio*/) { return NC_NOERR; }

static int MockUdfAbort(int /*ncid*/) { return NC_NOERR; }

static int MockUdfInqFilterAvail(int /*ncid*/, unsigned int id) {
  MockBzip2State& s = GetMockBzip2State();
  if (id != H5Z_FILTER_BZIP2) {
    return NC_ENOFILTER;
  }
  return s.avail_status;
}

static int MockUdfDefVarFilter(int /*ncid*/, int /*varid*/, unsigned int id,
                               size_t nparams, const unsigned int* params) {
  MockBzip2State& s = GetMockBzip2State();
  if (s.def_status != NC_NOERR) {
    return s.def_status;
  }
  if (id != H5Z_FILTER_BZIP2 || nparams != 1 || params == nullptr) {
    return NC_EFILTER;
  }
  s.has_bzip2 = true;
  s.reported_nparams = 1;
  s.level = params[0];
  return NC_NOERR;
}

static int MockUdfInqVarFilterIds(int /*ncid*/, int /*varid*/,
                                  size_t* nfiltersp, unsigned int* ids) {
  MockBzip2State& s = GetMockBzip2State();
  if (ids == nullptr) {
    if (s.inq_ids_first_status != NC_NOERR) {
      return s.inq_ids_first_status;
    }
    if (nfiltersp != nullptr) {
      *nfiltersp = s.has_bzip2 ? 1 : 0;
    }
    return NC_NOERR;
  }
  if (s.inq_ids_second_status != NC_NOERR) {
    return s.inq_ids_second_status;
  }
  if (nfiltersp != nullptr) {
    *nfiltersp = s.has_bzip2 ? 1 : 0;
  }
  if (s.has_bzip2) {
    ids[0] = H5Z_FILTER_BZIP2;
  }
  return NC_NOERR;
}

static int MockUdfInqVarFilterInfo(int /*ncid*/, int /*varid*/, unsigned int id,
                                   size_t* nparamsp, unsigned int* params) {
  MockBzip2State& s = GetMockBzip2State();
  ++s.inq_calls;
  if (params == nullptr) {
    if (s.first_inq_status != NC_NOERR) {
      return s.first_inq_status;
    }
    if (!s.has_bzip2 || id != H5Z_FILTER_BZIP2) {
      return NC_ENOFILTER;
    }
    if (nparamsp != nullptr) {
      *nparamsp = s.reported_nparams;
    }
    return NC_NOERR;
  }
  if (s.second_inq_status != NC_NOERR) {
    return s.second_inq_status;
  }
  if (nparamsp != nullptr) {
    *nparamsp = s.reported_nparams;
  }
  if (s.reported_nparams == 1) {
    params[0] = s.level;
  }
  return NC_NOERR;
}
}  // extern "C"

TEST(DfilterTest, InvalidNcidReturnsBadIdAcrossAllFilterApis) {
  constexpr int kBadIds[] = {-1, 999999};
  for (int bad_ncid : kBadIds) {
    SCOPED_TRACE(bad_ncid);
    size_t count = 0;
    unsigned int id = 0;
    unsigned int param = 0;
    int has_filter = 0;
    int level = 0;
    EXPECT_EQ(NC_EBADID, nc_inq_var_filter_ids(bad_ncid, 0, &count, nullptr));
    EXPECT_EQ(NC_EBADID, nc_inq_var_filter_info(bad_ncid, 0, H5Z_FILTER_DEFLATE,
                                                &count, &param));
    EXPECT_EQ(NC_EBADID,
              nc_def_var_filter(bad_ncid, 0, H5Z_FILTER_DEFLATE, 1, &param));
    EXPECT_EQ(NC_EBADID, nc_inq_var_filter(bad_ncid, 0, &id, &count, &param));
    EXPECT_EQ(NC_EBADID, nc_inq_filter_avail(bad_ncid, H5Z_FILTER_DEFLATE));
    EXPECT_EQ(NC_EBADID, nc_def_var_bzip2(bad_ncid, 0, 5));
    EXPECT_EQ(NC_EBADID, nc_inq_var_bzip2(bad_ncid, 0, &has_filter, &level));
  }
}

TEST(DfilterTest, ClassicFormatBehaviorAndNoOpStubs) {
  ScopedMemio memio;
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("classic_filter.nc", NC_CLOBBER, 1024, nc.ptr()));
  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 4, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v", NC_INT, 1, &dimid, &varid));

  EXPECT_EQ(NC_ENOFILTER, nc_inq_filter_avail(nc, H5Z_FILTER_DEFLATE));
  EXPECT_EQ(NC_ENOFILTER, nc_inq_filter_avail(nc, H5Z_FILTER_BZIP2));

  size_t nfilters = 99;
  EXPECT_EQ(NC_NOERR, nc_inq_var_filter_ids(nc, varid, &nfilters, nullptr));
  EXPECT_EQ(size_t{0}, nfilters);

  size_t nparams = 0;
  unsigned int param = 0;
  EXPECT_EQ(NC_ENOFILTER, nc_inq_var_filter_info(nc, varid, H5Z_FILTER_DEFLATE,
                                                 &nparams, &param));

  unsigned int filter_id = 999;
  EXPECT_EQ(NC_ENOTNC4,
            nc_def_var_filter(nc, varid, H5Z_FILTER_DEFLATE, 1, &param));
  EXPECT_EQ(NC_NOERR,
            nc_inq_var_filter(nc, varid, &filter_id, &nparams, &param));
  EXPECT_EQ(0U, filter_id);

  // Compiled-out zstandard/blosc helpers act as silent no-ops.
  EXPECT_EQ(NC_NOERR, nc_def_var_zstandard(nc, varid, 3));
  EXPECT_EQ(NC_NOERR, nc_inq_var_zstandard(nc, varid, nullptr, nullptr));
  EXPECT_EQ(NC_NOERR,
            nc_def_var_blosc(nc, varid, BLOSC_LZ4, 5, 0, BLOSC_SHUFFLE));
  EXPECT_EQ(NC_NOERR, nc_inq_var_blosc(nc, varid, nullptr, nullptr, nullptr,
                                       nullptr, nullptr));

  ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
}

TEST(DfilterTest, Netcdf4BuiltInFiltersRoundTripAndErrorPaths) {
  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("nc4_filter.nc", NC_NETCDF4, 4096, nc.ptr()));
    int dimid = -1;
    int plain_varid = -1;
    int filtered_varid = -1;
    int scalar_varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 8, &dimid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "plain", NC_INT, 1, &dimid, &plain_varid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "filtered", NC_INT, 1, &dimid, &filtered_varid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "scalar", NC_INT, 0, nullptr, &scalar_varid));

    // Built-in HDF5 filters are available; unbundled/unknown filters are not.
    EXPECT_EQ(NC_NOERR, nc_inq_filter_avail(nc, H5Z_FILTER_DEFLATE));
    EXPECT_EQ(NC_NOERR, nc_inq_filter_avail(nc, H5Z_FILTER_SHUFFLE));
    EXPECT_EQ(NC_NOERR, nc_inq_filter_avail(nc, H5Z_FILTER_FLETCHER32));
    EXPECT_EQ(NC_ENOFILTER, nc_inq_filter_avail(nc, H5Z_FILTER_BZIP2));
    EXPECT_EQ(NC_ENOFILTER, nc_inq_filter_avail(nc, 65432));

    // Unfiltered variable reports zero filters (both with and without idp).
    size_t nfilters = 99;
    EXPECT_EQ(NC_NOERR,
              nc_inq_var_filter_ids(nc, plain_varid, &nfilters, nullptr));
    EXPECT_EQ(size_t{0}, nfilters);
    unsigned int first_id = 777;
    EXPECT_EQ(NC_NOERR,
              nc_inq_var_filter(nc, plain_varid, &first_id, nullptr, nullptr));
    EXPECT_EQ(0U, first_id);
    EXPECT_EQ(NC_NOERR,
              nc_inq_var_filter(nc, plain_varid, nullptr, nullptr, nullptr));
    EXPECT_EQ(NC_ENOFILTER,
              nc_inq_var_filter_info(nc, plain_varid, H5Z_FILTER_DEFLATE,
                                     nullptr, nullptr));

    // Error paths inside NetCDF-4 filter APIs.
    constexpr int kBadVarId = 99;
    EXPECT_EQ(NC_ENOTVAR,
              nc_inq_var_filter_ids(nc, kBadVarId, &nfilters, nullptr));
    EXPECT_EQ(NC_ENOTVAR,
              nc_inq_var_filter_info(nc, kBadVarId, H5Z_FILTER_DEFLATE, nullptr,
                                     nullptr));
    EXPECT_EQ(NC_ENOTVAR,
              nc_def_var_filter(nc, kBadVarId, H5Z_FILTER_DEFLATE, 0, nullptr));
    EXPECT_EQ(NC_ENOTVAR,
              nc_inq_var_filter(nc, kBadVarId, &first_id, nullptr, nullptr));
    EXPECT_EQ(NC_EINVAL, nc_def_var_filter(nc, scalar_varid, H5Z_FILTER_DEFLATE,
                                           0, nullptr));
    EXPECT_EQ(NC_ENOFILTER,
              nc_def_var_filter(nc, filtered_varid, 65432, 0, nullptr));

    // bzip2 unavailability check on real NetCDF-4 datasets.
    int has_bz2 = -1;
    int bz2_level = -1;
    EXPECT_EQ(NC_ENOFILTER, nc_def_var_bzip2(nc, filtered_varid, 5));
    EXPECT_EQ(NC_ENOFILTER,
              nc_inq_var_bzip2(nc, filtered_varid, &has_bz2, &bz2_level));
    EXPECT_EQ(0, has_bz2);
    EXPECT_EQ(0, bz2_level);

    // Attach shuffle + deflate filters via nc_def_var_filter().
    const std::array<size_t, 1> chunks = {4};
    ASSERT_EQ(NC_NOERR, nc_def_var_chunking(nc, filtered_varid, NC_CHUNKED,
                                            chunks.data()));
    ASSERT_EQ(NC_NOERR, nc_def_var_filter(nc, filtered_varid,
                                          H5Z_FILTER_SHUFFLE, 0, nullptr));
    constexpr unsigned int kDeflateLevel = 6;
    ASSERT_EQ(NC_NOERR,
              nc_def_var_filter(nc, filtered_varid, H5Z_FILTER_DEFLATE, 1,
                                &kDeflateLevel));

    constexpr std::array<int, 8> kData = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, filtered_varid, kData.data()));
    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  // Reopen in-memory NetCDF-4 file and inspect persisted filter metadata.
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_open_mem("nc4_filter.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, nc.ptr()));
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(nc, "filtered", &varid));

  size_t nfilters = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_var_filter_ids(nc, varid, &nfilters, nullptr));
  ASSERT_EQ(size_t{2}, nfilters);
  std::vector<unsigned int> ids(nfilters, 0);
  ASSERT_EQ(NC_NOERR, nc_inq_var_filter_ids(nc, varid, nullptr, ids.data()));
  EXPECT_THAT(ids,
              ::testing::ElementsAre(H5Z_FILTER_SHUFFLE, H5Z_FILTER_DEFLATE));

  // nc_inq_var_filter() returns the first filter (shuffle, 1 param auto-added).
  unsigned int first_id = 0;
  size_t first_nparams = 0;
  std::array<unsigned int, 4> first_params{};
  ASSERT_EQ(NC_NOERR, nc_inq_var_filter(nc, varid, &first_id, &first_nparams,
                                        first_params.data()));
  EXPECT_EQ(H5Z_FILTER_SHUFFLE, first_id);
  EXPECT_EQ(size_t{1}, first_nparams);

  // Also check null idp on a variable with filters.
  ASSERT_EQ(NC_NOERR, nc_inq_var_filter(nc, varid, nullptr, &first_nparams,
                                        first_params.data()));

  size_t deflate_nparams = 0;
  unsigned int deflate_param = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_var_filter_info(nc, varid, H5Z_FILTER_DEFLATE,
                                             &deflate_nparams, &deflate_param));
  EXPECT_EQ(size_t{1}, deflate_nparams);
  EXPECT_EQ(6U, deflate_param);

  std::array<int, 8> actual{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, varid, actual.data()));
  EXPECT_EQ((std::array<int, 8>{1, 2, 3, 4, 5, 6, 7, 8}), actual);
}

TEST(DfilterTest, Bzip2BranchesCoveredWithMockUdfDispatcher) {
  RunInExecveChild([]() -> bool {
    MockBzip2State& state = GetMockBzip2State();
    state.reset();

    NC_Dispatch mock_dispatch{};
    mock_dispatch.model = NC_FORMATX_UDF0;
    mock_dispatch.dispatch_version = NC_DISPATCH_VERSION;
    mock_dispatch.create = &MockUdfCreate;
    mock_dispatch.close = &MockUdfClose;
    mock_dispatch.abort = &MockUdfAbort;
    mock_dispatch.def_var_filter = &MockUdfDefVarFilter;
    mock_dispatch.inq_var_filter_ids = &MockUdfInqVarFilterIds;
    mock_dispatch.inq_var_filter_info = &MockUdfInqVarFilterInfo;
    mock_dispatch.inq_filter_avail = &MockUdfInqFilterAvail;

    if (nc_def_user_format(NC_UDF0, &mock_dispatch, nullptr) != NC_NOERR) {
      return false;
    }

    ScopedNcFile nc;
    if (nc_create("udf_bzip2.nc", NC_UDF0, nc.ptr()) != NC_NOERR) {
      return false;
    }

    // Inquire when bzip2 is available globally, but not set on the variable.
    int has_filter = -1;
    int level = -1;
    if (nc_inq_var_bzip2(nc, 0, &has_filter, &level) != NC_NOERR ||
        has_filter != 0 || level != 0) {
      return false;
    }
    // Null output pointers when filter is absent.
    if (nc_inq_var_bzip2(nc, 0, nullptr, nullptr) != NC_NOERR) {
      return false;
    }

    // Level range validation after availability check passes.
    if (nc_def_var_bzip2(nc, 0, 0) != NC_EINVAL ||
        nc_def_var_bzip2(nc, 0, -3) != NC_EINVAL ||
        nc_def_var_bzip2(nc, 0, 10) != NC_EINVAL) {
      return false;
    }

    // Error propagation from dispatch def_var_filter.
    state.def_status = NC_EFILTER;
    if (nc_def_var_bzip2(nc, 0, 5) != NC_EFILTER) {
      return false;
    }
    state.def_status = NC_NOERR;

    // Valid define and inquire round-trip (min=1, mid=6, max=9).
    if (nc_def_var_bzip2(nc, 0, 1) != NC_NOERR ||
        nc_def_var_bzip2(nc, 0, 9) != NC_NOERR ||
        nc_def_var_bzip2(nc, 0, 6) != NC_NOERR) {
      return false;
    }
    has_filter = -1;
    level = -1;
    if (nc_inq_var_bzip2(nc, 0, &has_filter, &level) != NC_NOERR ||
        has_filter != 1 || level != 6) {
      return false;
    }
    if (nc_inq_var_bzip2(nc, 0, nullptr, nullptr) != NC_NOERR) {
      return false;
    }

    // Exercise nc_inq_var_filter() success and internal error propagation
    // paths.
    unsigned int fid = 0;
    size_t f_nparams = 0;
    unsigned int f_param = 0;
    if (nc_inq_var_filter(nc, 0, &fid, &f_nparams, &f_param) != NC_NOERR ||
        fid != H5Z_FILTER_BZIP2 || f_nparams != 1 || f_param != 6) {
      return false;
    }
    state.inq_ids_first_status = NC_ENOTVAR;
    if (nc_inq_var_filter(nc, 0, &fid, &f_nparams, &f_param) != NC_ENOTVAR) {
      return false;
    }
    state.inq_ids_first_status = NC_NOERR;
    state.inq_ids_second_status = NC_EINVAL;
    if (nc_inq_var_filter(nc, 0, &fid, &f_nparams, &f_param) != NC_EINVAL) {
      return false;
    }
    state.inq_ids_second_status = NC_NOERR;
    state.second_inq_status = NC_EHDFERR;
    if (nc_inq_var_filter(nc, 0, &fid, &f_nparams, &f_param) != NC_EHDFERR) {
      return false;
    }
    state.second_inq_status = NC_NOERR;

    // Non-1 nparams triggers NC_EFILTER while still reporting has_filter == 1.
    state.reported_nparams = 2;
    has_filter = -1;
    level = -1;
    if (nc_inq_var_bzip2(nc, 0, &has_filter, &level) != NC_EFILTER ||
        has_filter != 1) {
      return false;
    }
    state.reported_nparams = 0;
    if (nc_inq_var_bzip2(nc, 0, &has_filter, &level) != NC_EFILTER ||
        has_filter != 1) {
      return false;
    }
    state.reported_nparams = 1;

    // First inquiry error != NC_ENOFILTER propagates directly.
    state.first_inq_status = NC_ENOTVAR;
    if (nc_inq_var_bzip2(nc, 0, &has_filter, &level) != NC_ENOTVAR) {
      return false;
    }
    state.first_inq_status = NC_NOERR;

    // Second inquiry error (fetching parameters) propagates cleanly.
    state.second_inq_status = NC_EHDFERR;
    if (nc_inq_var_bzip2(nc, 0, &has_filter, &level) != NC_EHDFERR) {
      return false;
    }
    state.second_inq_status = NC_NOERR;

    // Filter availability failure inside nc_inq_var_bzip2.
    state.avail_status = NC_ENOFILTER;
    has_filter = -1;
    level = -1;
    if (nc_inq_var_bzip2(nc, 0, &has_filter, &level) != NC_ENOFILTER ||
        has_filter != 0 || level != 0) {
      return false;
    }

    return state.inq_calls > 0 && nc.close() == NC_NOERR;
  });
}

}  // namespace
}  // namespace netcdf
