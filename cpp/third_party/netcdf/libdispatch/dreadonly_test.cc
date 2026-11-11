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

// Unit tests for //third_party/netcdf/libdispatch/dreadonly.c.
//
// Covers all NC_RO_* read-only dispatch stubs (returning NC_EPERM for
// mutating operations and NC_NOERR for NC_RO__enddef and NC_RO_sync) and
// verifies NC_Dispatch function pointer compatibility.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dreadonly.c

#include <array>
#include <cstddef>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

extern "C" {
#include "third_party/netcdf/include/ncdispatch.h"
}

namespace netcdf {
namespace {

TEST(DreadonlyTest, AttributeAndMetadataMutationsReturnEperm) {
  int att_val = 7;
  int out_dimid = -1;
  int out_varid = -1;
  int out_old_mode = -1;
  constexpr int kDimIds[1] = {0};

  EXPECT_EQ(NC_EPERM, NC_RO_rename_att(0, NC_GLOBAL, "old", "new"));
  EXPECT_EQ(NC_EPERM, NC_RO_del_att(0, 0, "attr"));
  EXPECT_EQ(NC_EPERM,
            NC_RO_put_att(0, NC_GLOBAL, "attr", NC_INT, 1, &att_val, NC_INT));

  EXPECT_EQ(NC_EPERM, NC_RO_def_dim(0, "x", 10, &out_dimid));
  EXPECT_EQ(-1, out_dimid);
  EXPECT_EQ(NC_EPERM, NC_RO_rename_dim(0, 0, "y"));

  EXPECT_EQ(NC_EPERM, NC_RO_def_var(0, "v", NC_DOUBLE, 1, kDimIds, &out_varid));
  EXPECT_EQ(-1, out_varid);
  EXPECT_EQ(NC_EPERM, NC_RO_def_var_fill(0, 0, NC_FILL, &att_val));
  EXPECT_EQ(NC_EPERM, NC_RO_rename_var(0, 0, "w"));

  EXPECT_EQ(NC_EPERM, NC_RO_set_fill(0, NC_NOFILL, &out_old_mode));
  EXPECT_EQ(-1, out_old_mode);
  EXPECT_EQ(NC_EPERM, NC_RO_redef(0));
}

TEST(DreadonlyTest, PutVaraAndCreateReturnEpermAndNoopSyncEnddefReturnNoerr) {
  constexpr std::array<size_t, 1> kStart = {0};
  constexpr std::array<size_t, 1> kCount = {2};
  constexpr std::array<int, 2> kData = {1, 2};
  size_t chunk_hint = 4096;

  EXPECT_EQ(NC_EPERM, NC_RO_put_vara(0, 0, kStart.data(), kCount.data(),
                                     kData.data(), NC_INT));
  EXPECT_EQ(NC_EPERM, NC_RO_create("ro.nc", NC_CLOBBER, 0, 0, &chunk_hint,
                                   nullptr, nullptr, 0));
  EXPECT_EQ(4096u, chunk_hint);

  // Read-only enddef and sync succeed as no-ops.
  EXPECT_EQ(NC_NOERR, NC_RO__enddef(0, 0, 0, 0, 0));
  EXPECT_EQ(NC_NOERR, NC_RO_sync(0));
}

TEST(DreadonlyTest, NullArgumentsAreSafelyHandled) {
  EXPECT_EQ(NC_EPERM, NC_RO_rename_att(-1, 0, nullptr, nullptr));
  EXPECT_EQ(NC_EPERM, NC_RO_del_att(-1, 0, nullptr));
  EXPECT_EQ(NC_EPERM,
            NC_RO_put_att(-1, 0, nullptr, NC_NAT, 0, nullptr, NC_NAT));
  EXPECT_EQ(NC_EPERM, NC_RO_def_dim(-1, nullptr, 0, nullptr));
  EXPECT_EQ(NC_EPERM, NC_RO_rename_dim(-1, 0, nullptr));
  EXPECT_EQ(NC_EPERM, NC_RO_def_var(-1, nullptr, NC_NAT, 0, nullptr, nullptr));
  EXPECT_EQ(NC_EPERM, NC_RO_def_var_fill(-1, 0, 0, nullptr));
  EXPECT_EQ(NC_EPERM, NC_RO_rename_var(-1, 0, nullptr));
  EXPECT_EQ(NC_EPERM, NC_RO_put_vara(-1, 0, nullptr, nullptr, nullptr, NC_NAT));
  EXPECT_EQ(NC_EPERM, NC_RO_set_fill(-1, 0, nullptr));
  EXPECT_EQ(NC_EPERM, NC_RO_redef(-1));
  EXPECT_EQ(NC_NOERR, NC_RO__enddef(-1, 1, 2, 3, 4));
  EXPECT_EQ(NC_NOERR, NC_RO_sync(-1));
  EXPECT_EQ(NC_EPERM,
            NC_RO_create(nullptr, 0, 0, 0, nullptr, nullptr, nullptr, -1));
}

TEST(DreadonlyTest, DispatchTableAcceptsAllReadOnlyStubs) {
  NC_Dispatch ro_table{};
  ro_table.create = NC_RO_create;
  ro_table.redef = NC_RO_redef;
  ro_table._enddef = NC_RO__enddef;
  ro_table.sync = NC_RO_sync;
  ro_table.set_fill = NC_RO_set_fill;
  ro_table.def_dim = NC_RO_def_dim;
  ro_table.rename_dim = NC_RO_rename_dim;
  ro_table.rename_att = NC_RO_rename_att;
  ro_table.del_att = NC_RO_del_att;
  ro_table.put_att = NC_RO_put_att;
  ro_table.def_var = NC_RO_def_var;
  ro_table.rename_var = NC_RO_rename_var;
  ro_table.put_vara = NC_RO_put_vara;
  ro_table.def_var_fill = NC_RO_def_var_fill;

  EXPECT_EQ(NC_EPERM,
            ro_table.create("x.nc", 0, 0, 0, nullptr, nullptr, &ro_table, 0));
  EXPECT_EQ(NC_EPERM, ro_table.redef(0));
  EXPECT_EQ(NC_NOERR, ro_table._enddef(0, 0, 0, 0, 0));
  EXPECT_EQ(NC_NOERR, ro_table.sync(0));
  EXPECT_EQ(NC_EPERM, ro_table.set_fill(0, 0, nullptr));
  EXPECT_EQ(NC_EPERM, ro_table.def_dim(0, "d", 1, nullptr));
  EXPECT_EQ(NC_EPERM, ro_table.rename_dim(0, 0, "d2"));
  EXPECT_EQ(NC_EPERM, ro_table.rename_att(0, 0, "a", "b"));
  EXPECT_EQ(NC_EPERM, ro_table.del_att(0, 0, "a"));
  EXPECT_EQ(NC_EPERM, ro_table.put_att(0, 0, "a", NC_INT, 0, nullptr, NC_INT));
  EXPECT_EQ(NC_EPERM, ro_table.def_var(0, "v", NC_INT, 0, nullptr, nullptr));
  EXPECT_EQ(NC_EPERM, ro_table.rename_var(0, 0, "v2"));
  EXPECT_EQ(NC_EPERM,
            ro_table.put_vara(0, 0, nullptr, nullptr, nullptr, NC_INT));
  EXPECT_EQ(NC_EPERM, ro_table.def_var_fill(0, 0, 0, nullptr));
}

}  // namespace
}  // namespace netcdf
