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

// Unit tests for //third_party/netcdf/libdispatch/nc.c.
//
// Exercises NC_check_id, new_NC/free_NC lifecycle management, and
// nc_set_default_format/nc_get_default_format validation and persistence.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/nc.c

#include <cstdlib>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_dispatch.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

extern "C" {
#include "third_party/netcdf/include/nc.h"
}  // extern "C"

namespace netcdf {
namespace {

using ScopedRawNc = std::unique_ptr<NC, decltype(&free_NC)>;

TEST(NcTest, NewAndFreeNcHandleLifecycleAndErrors) {
  // free_NC(nullptr) is a safe no-op.
  free_NC(nullptr);

  // free_NC handles an NC struct whose path is nullptr.
  ScopedRawNc empty_nc(static_cast<NC*>(std::calloc(1, sizeof(NC))), &free_NC);
  ASSERT_NE(nullptr, empty_nc.get());
  empty_nc.reset();

  NC_Dispatch dummy_dispatch{};
  dummy_dispatch.model = NC_FORMATX_NC3;
  dummy_dispatch.dispatch_version = NC_DISPATCH_VERSION;

  // Normal allocation populates fields and is released cleanly by free_NC.
  NC* raw_ncp = nullptr;
  ASSERT_EQ(NC_NOERR, new_NC(&dummy_dispatch, "mem/sample_dataset.nc",
                             NC_NOWRITE, &raw_ncp));
  ScopedRawNc ncp(raw_ncp, &free_NC);
  ASSERT_NE(nullptr, ncp.get());
  EXPECT_EQ(&dummy_dispatch, ncp->dispatch);
  EXPECT_STREQ("mem/sample_dataset.nc", ncp->path);
  EXPECT_EQ(NC_NOWRITE, ncp->mode);
  EXPECT_EQ(0, ncp->ext_ncid);
  EXPECT_EQ(0, ncp->int_ncid);
  EXPECT_EQ(nullptr, ncp->dispatchdata);
  ncp.reset();

  // Passing nullptr for path triggers nulldup failure cleanup and NC_ENOMEM.
  NC* bad_ncp = nullptr;
  EXPECT_EQ(NC_ENOMEM, new_NC(&dummy_dispatch, nullptr, NC_WRITE, &bad_ncp));
  EXPECT_EQ(nullptr, bad_ncp);

  // Passing nullptr for ncpp allocates, validates, frees internally, and
  // succeeds.
  EXPECT_EQ(NC_NOERR, new_NC(&dummy_dispatch, "discarded_output.nc", NC_CLOBBER,
                             nullptr));

  // Passing nullptr dispatcher with a valid path succeeds.
  NC* raw_no_disp_ncp = nullptr;
  ASSERT_EQ(NC_NOERR, new_NC(nullptr, "no_dispatch.nc", 0, &raw_no_disp_ncp));
  ScopedRawNc no_disp_ncp(raw_no_disp_ncp, &free_NC);
  ASSERT_NE(nullptr, no_disp_ncp.get());
  EXPECT_EQ(nullptr, no_disp_ncp->dispatch);
  EXPECT_STREQ("no_dispatch.nc", no_disp_ncp->path);
}

TEST(NcTest, CheckIdResolvesOpenClassicAndNetcdf4IdsAndRejectsInvalidIds) {
  NC* found = nullptr;
  EXPECT_EQ(NC_EBADID, NC_check_id(-1, &found));
  EXPECT_EQ(nullptr, found);
  EXPECT_EQ(NC_EBADID, NC_check_id(0, &found));
  EXPECT_EQ(NC_EBADID, NC_check_id(0x12340000, nullptr));

  // Open an in-memory classic dataset and verify NC_check_id behavior.
  ScopedNcFile classic_nc;
  ASSERT_EQ(NC_NOERR, nc_create_mem("nc_check_classic.nc", NC_CLOBBER, 1024,
                                    classic_nc.ptr()));
  const int classic_id = classic_nc.get();
  ASSERT_EQ(NC_NOERR, NC_check_id(classic_id, &found));
  ASSERT_NE(nullptr, found);
  EXPECT_EQ(classic_id, found->ext_ncid);
  EXPECT_STREQ("nc_check_classic.nc", found->path);

  // Passing ncpp == nullptr still validates the ncid.
  EXPECT_EQ(NC_NOERR, NC_check_id(classic_id, nullptr));

  // Classic datasets reject non-zero group bits in the lower 16 bits.
  EXPECT_EQ(NC_EBADID, NC_check_id(classic_id | 1, &found));

  ASSERT_EQ(NC_NOERR, classic_nc.close());
  EXPECT_EQ(NC_EBADID, NC_check_id(classic_id, &found));

  // Open an in-memory netCDF-4 dataset with a subgroup and verify group lookup.
  ScopedNcFile nc4_nc;
  ASSERT_EQ(NC_NOERR, nc_create_mem("nc_check_nc4.nc", NC_NETCDF4 | NC_CLOBBER,
                                    4096, nc4_nc.ptr()));
  int subgrp_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(nc4_nc.get(), "subgrp", &subgrp_id));

  NC* grp_owner = nullptr;
  ASSERT_EQ(NC_NOERR, NC_check_id(subgrp_id, &grp_owner));
  ASSERT_NE(nullptr, grp_owner);
  EXPECT_EQ(nc4_nc.get(), grp_owner->ext_ncid);

  ASSERT_EQ(NC_NOERR, nc4_nc.close());
  EXPECT_EQ(NC_EBADID, NC_check_id(subgrp_id, &grp_owner));
}

TEST(NcTest, DefaultFormatAcceptsValidFormatsRejectsInvalidAndAffectsCreate) {
  ScopedDefaultFormat restore_format(NC_FORMAT_CLASSIC);
  const int initial_format = restore_format.previous_format();
  ASSERT_EQ(NC_NOERR, nc_set_default_format(initial_format, nullptr));

  constexpr int kValidFormats[] = {
      NC_FORMAT_CLASSIC, NC_FORMAT_64BIT_OFFSET,    NC_FORMAT_CDF5,
      NC_FORMAT_NETCDF4, NC_FORMAT_NETCDF4_CLASSIC,
  };

  int expected_prev = initial_format;
  for (const int fmt : kValidFormats) {
    SCOPED_TRACE(fmt);
    int old_fmt = -1;
    ASSERT_EQ(NC_NOERR, nc_set_default_format(fmt, &old_fmt));
    EXPECT_EQ(expected_prev, old_fmt);
    EXPECT_EQ(fmt, nc_get_default_format());
    EXPECT_EQ(NC_NOERR, nc_set_default_format(fmt, nullptr));
    EXPECT_EQ(fmt, nc_get_default_format());
    expected_prev = fmt;
  }

  // Invalid format codes return NC_EINVAL without mutating
  // default_create_format, while still populating old_formatp if non-null.
  constexpr int kInvalidFormats[] = {0, -1, -999, 6, 99, NC_FORMATX_UDF0};
  for (const int bad_fmt : kInvalidFormats) {
    SCOPED_TRACE(bad_fmt);
    int old_fmt = -1;
    EXPECT_EQ(NC_EINVAL, nc_set_default_format(bad_fmt, &old_fmt));
    EXPECT_EQ(expected_prev, old_fmt);
    EXPECT_EQ(expected_prev, nc_get_default_format());
    EXPECT_EQ(NC_EINVAL, nc_set_default_format(bad_fmt, nullptr));
    EXPECT_EQ(expected_prev, nc_get_default_format());
  }

  // Verify default format actually controls format when cmode == 0.
  ASSERT_EQ(NC_NOERR, nc_set_default_format(NC_FORMAT_64BIT_OFFSET, nullptr));
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create_mem("default_64bit.nc", 0, 1024, nc.ptr()));
    int actual_fmt = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_format(nc.get(), &actual_fmt));
    EXPECT_EQ(NC_FORMAT_64BIT_OFFSET, actual_fmt);
  }

  ASSERT_EQ(NC_NOERR, nc_set_default_format(NC_FORMAT_NETCDF4, nullptr));
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create_mem("default_nc4.nc", 0, 4096, nc.ptr()));
    int actual_fmt = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_format(nc.get(), &actual_fmt));
    EXPECT_EQ(NC_FORMAT_NETCDF4, actual_fmt);
  }
}

}  // namespace
}  // namespace netcdf
