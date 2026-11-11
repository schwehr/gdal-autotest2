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

// Unit tests for //third_party/netcdf/libsrc4/nc4dim.c.
//
// Covers NC4_inq_unlimdim(), NC4_inq_dimid(), and NC4_inq_unlimdims()
// including fixed vs. multiple unlimited dimensions, nested group scoping and
// ancestor inheritance, shadowing, fully-qualified (/grp/dim) paths, Unicode
// NFC/NFD normalization, optional NULL out-pointers, memory reopen persistence,
// and error checks.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libsrc4/nc4dim.c

#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::ElementsAre;

TEST(Nc4DimTest, InqUnlimdimReturnsMinusOneWhenAbsentAndFirstUnlimited) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create("nc4dim_unlimdim.nc", NC_NETCDF4 | NC_DISKLESS,
                                file.ptr()));

  // NULL unlimdimidp must succeed cleanly.
  EXPECT_EQ(NC_NOERR, nc_inq_unlimdim(file, nullptr));

  int unlim_id = 123;
  ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(file, &unlim_id));
  EXPECT_EQ(-1, unlim_id);

  // Define only a fixed dimension first.
  int fixed_dim = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "fixed", 4, &fixed_dim));
  unlim_id = 123;
  ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(file, &unlim_id));
  EXPECT_EQ(-1, unlim_id);

  // Add two unlimited dimensions; NC4_inq_unlimdim() skips fixed_dim and
  // returns the first unlimited dimension.
  int unlim_first = -1;
  int unlim_second = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "time", NC_UNLIMITED, &unlim_first));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "step", NC_UNLIMITED, &unlim_second));
  ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(file, &unlim_id));
  EXPECT_EQ(unlim_first, unlim_id);

  // Child group with no dimensions of its own traverses g->parent loop and
  // currently returns -1 because NC4_inq_unlimdim() inspects grp->dim.
  int child_grp = -1;
  int empty_nested_grp = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(file, "child", &child_grp));
  ASSERT_EQ(NC_NOERR, nc_def_grp(child_grp, "empty_nested", &empty_nested_grp));
  int nested_unlim = 123;
  ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(empty_nested_grp, &nested_unlim));
  EXPECT_EQ(-1, nested_unlim);

  // Child group with its own fixed + unlimited dims returns its first
  // unlimited dimension.
  int sub_fixed = -1;
  int sub_unlim = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(child_grp, "sub_fixed", 3, &sub_fixed));
  ASSERT_EQ(NC_NOERR,
            nc_def_dim(child_grp, "sub_unlim", NC_UNLIMITED, &sub_unlim));
  int child_unlim = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(child_grp, &child_unlim));
  EXPECT_EQ(sub_unlim, child_unlim);
}

TEST(Nc4DimTest, InqUnlimdimsEnumeratesAndRoundTripsMultipleUnlimitedDims) {
  ScopedMemio memio;
  int root_unlim0 = -1;
  int root_unlim1 = -1;
  int root_unlim2 = -1;
  int sub_unlim = -1;

  {
    ScopedNcFile writer;
    ASSERT_EQ(NC_NOERR, nc_create_mem("nc4dim_unlimdims.nc", NC_NETCDF4, 0,
                                      writer.ptr()));

    // No unlimited dims initially.
    int count = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(writer, &count, nullptr));
    EXPECT_EQ(0, count);

    int fixed0 = -1;
    int fixed1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(writer, "fixed0", 2, &fixed0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(writer, "u0", NC_UNLIMITED, &root_unlim0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(writer, "fixed1", 5, &fixed1));
    ASSERT_EQ(NC_NOERR, nc_def_dim(writer, "u1", NC_UNLIMITED, &root_unlim1));
    ASSERT_EQ(NC_NOERR, nc_def_dim(writer, "u2", NC_UNLIMITED, &root_unlim2));

    // All pointer combinations: (NULL, NULL), (&count, NULL), (NULL, ids),
    // and (&count, ids).
    EXPECT_EQ(NC_NOERR, nc_inq_unlimdims(writer, nullptr, nullptr));

    count = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(writer, &count, nullptr));
    ASSERT_EQ(3, count);

    std::vector<int> ids_only(3, -999);
    ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(writer, nullptr, ids_only.data()));
    EXPECT_THAT(ids_only, ElementsAre(root_unlim0, root_unlim1, root_unlim2));

    count = -1;
    std::vector<int> both_ids(3, -999);
    ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(writer, &count, both_ids.data()));
    EXPECT_EQ(3, count);
    EXPECT_THAT(both_ids, ElementsAre(root_unlim0, root_unlim1, root_unlim2));

    int sub_grp = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(writer, "sub", &sub_grp));
    ASSERT_EQ(NC_NOERR, nc_def_dim(sub_grp, "sub_u", NC_UNLIMITED, &sub_unlim));
    int sub_count = -1;
    std::vector<int> sub_ids(1, -999);
    ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(sub_grp, &sub_count, sub_ids.data()));
    EXPECT_EQ(1, sub_count);
    EXPECT_THAT(sub_ids, ElementsAre(sub_unlim));

    ASSERT_EQ(NC_NOERR, nc_close_memio(writer.release(), memio.get()));
  }

  // Reopen from memory and verify unlimited dimensions persist across HDF5
  // serialization.
  ScopedNcFile reader;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("nc4dim_unlimdims.nc", NC_NOWRITE, memio.size(),
                        memio.get()->memory, reader.ptr()));
  int count = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(reader, &count, nullptr));
  ASSERT_EQ(3, count);
  std::vector<int> reopened_ids(3, -999);
  ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(reader, &count, reopened_ids.data()));
  EXPECT_EQ(3, count);

  int got_u0 = -1;
  int got_u1 = -1;
  int got_u2 = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(reader, "u0", &got_u0));
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(reader, "u1", &got_u1));
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(reader, "u2", &got_u2));
  EXPECT_THAT(reopened_ids, ElementsAre(got_u0, got_u1, got_u2));

  int first_unlim = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(reader, &first_unlim));
  EXPECT_EQ(got_u0, first_unlim);
}

TEST(Nc4DimTest, InqDimidResolvesLocalInheritedShadowedUnicodeAndFqnNames) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create("nc4dim_inq_dimid.nc", NC_NETCDF4 | NC_DISKLESS,
                                file.ptr()));

  int root_lat = -1;
  int root_time = -1;
  int root_cafe = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "lat", 10, &root_lat));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "time", NC_UNLIMITED, &root_time));
  // NFC U+00E9 ("café_dim").
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "caf\xc3\xa9_dim", 4, &root_cafe));

  int sub_grp = -1;
  int nested_grp = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(file, "sub", &sub_grp));
  ASSERT_EQ(NC_NOERR, nc_def_grp(sub_grp, "nested", &nested_grp));

  int sub_lat = -1;
  int sub_dim = -1;
  int deep_dim = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(sub_grp, "lat", 20, &sub_lat));
  ASSERT_EQ(NC_NOERR, nc_def_dim(sub_grp, "sub_dim", 6, &sub_dim));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nested_grp, "deep_dim", 7, &deep_dim));

  // Optional NULL idp returns NC_NOERR when found.
  EXPECT_EQ(NC_NOERR, nc_inq_dimid(file, "lat", nullptr));

  // Local lookup in root.
  int id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(file, "lat", &id));
  EXPECT_EQ(root_lat, id);

  // Ancestor lookup from nested child finds grandparent dimension.
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(nested_grp, "time", &id));
  EXPECT_EQ(root_time, id);

  // Child dimension shadows root dimension of the same name.
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(nested_grp, "lat", &id));
  EXPECT_EQ(sub_lat, id);
  EXPECT_NE(root_lat, sub_lat);

  // NFD "cafe\xcc\x81_dim" normalizes to match NFC "caf\xc3\xa9_dim".
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(file, "cafe\xcc\x81_dim", &id));
  EXPECT_EQ(root_cafe, id);

  // Fully qualified names resolve from both root and subgroup ncids.
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(file, "/sub/sub_dim", &id));
  EXPECT_EQ(sub_dim, id);
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(nested_grp, "/sub/sub_dim", &id));
  EXPECT_EQ(sub_dim, id);
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(file, "/sub/nested/deep_dim", &id));
  EXPECT_EQ(deep_dim, id);
}

TEST(Nc4DimTest, DimQueriesRejectInvalidIdsNamesAndMissingTargets) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create("nc4dim_errors.nc", NC_NETCDF4 | NC_DISKLESS,
                                file.ptr()));
  int lat_dim = -1;
  int sub_grp = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "lat", 5, &lat_dim));
  ASSERT_EQ(NC_NOERR, nc_def_grp(file, "sub", &sub_grp));

  int out = -1;
  const int bogus_grpid = file.get() | 0x7FFF;

  // Invalid ncid checks for all three entry points.
  EXPECT_EQ(NC_EBADID, nc_inq_unlimdim(-1, &out));
  EXPECT_EQ(NC_EBADID, nc_inq_unlimdim(bogus_grpid, &out));
  EXPECT_EQ(NC_EBADID, nc_inq_unlimdims(-1, &out, nullptr));
  EXPECT_EQ(NC_EBADID, nc_inq_unlimdims(bogus_grpid, &out, nullptr));
  EXPECT_EQ(NC_EBADID, nc_inq_dimid(-1, "lat", &out));
  EXPECT_EQ(NC_EBADID, nc_inq_dimid(bogus_grpid, "lat", &out));

  // NC4_inq_dimid() input validation, name normalization, and FQN errors.
  EXPECT_EQ(NC_EINVAL, nc_inq_dimid(file, nullptr, &out));
  EXPECT_EQ(NC_EINVAL, nc_inq_dimid(file, "sub/lat", &out));
  EXPECT_EQ(NC_EINVAL, nc_inq_dimid(file, "/", &out));
  EXPECT_EQ(NC_EINVAL, nc_inq_dimid(file, "/lat", &out));

  EXPECT_EQ(NC_EBADNAME, nc_inq_dimid(file, "\xff\xfe", &out));
  const std::string overlong_name(NC_MAX_NAME + 1, 'a');
  EXPECT_EQ(NC_EMAXNAME, nc_inq_dimid(file, overlong_name.c_str(), &out));

  EXPECT_EQ(NC_ENOGRP, nc_inq_dimid(file, "/missing_group/lat", &out));
  // Upstream NC4_inq_dimid() returns NC_EBADTYPE when a valid FQN group does
  // not contain the named dimension.
  EXPECT_EQ(NC_EBADTYPE, nc_inq_dimid(file, "/sub/missing_dim", &out));
  EXPECT_EQ(NC_EBADDIM, nc_inq_dimid(file, "missing_dim", &out));
}

}  // namespace
}  // namespace netcdf
