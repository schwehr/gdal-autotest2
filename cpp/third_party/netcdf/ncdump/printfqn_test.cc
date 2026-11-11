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

// Unit tests for //third_party/netcdf/ncdump/printfqn.c.
//
// Verifies fully qualified dimension (-d) and user-defined type (-t) name
// resolution across root, sibling, and nested netCDF-4 groups, usage/option
// handling (-D/-V), and error exit behavior inside isolated forked children.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/printfqn.c

#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/cli_mains.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
// clang-format on

namespace netcdf {
namespace {

void CreateHierarchyFile(const char* path) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create(path, NC_NETCDF4 | NC_CLOBBER, nc.ptr()));

  int root_dim = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "root_dim", 3, &root_dim));

  nc_type root_enum = 0;
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_INT, "root_enum", &root_enum));
  const int enum_val = 0;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), root_enum, "E0", &enum_val));

  int root_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "root_var", root_enum, 1, &root_dim,
                                 &root_var));
  int root_atomic = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "root_atomic", NC_INT, 1, &root_dim,
                                 &root_atomic));

  // Define /grp_first before /grp_second so searches for objects in
  // /grp_second or /grp_second/leaf traverse /grp_first and handle miss codes.
  int grp_first = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "grp_first", &grp_first));
  int first_dim = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(grp_first, "first_dim", 2, &first_dim));
  nc_type first_opaque = 0;
  ASSERT_EQ(NC_NOERR,
            nc_def_opaque(grp_first, 4, "first_opaque", &first_opaque));
  int first_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(grp_first, "first_var", first_opaque, 1,
                                 &first_dim, &first_var));

  int grp_second = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "grp_second", &grp_second));
  int sec_dim = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(grp_second, "sec_dim", 4, &sec_dim));
  nc_type sec_cmpd = 0;
  ASSERT_EQ(NC_NOERR,
            nc_def_compound(grp_second, sizeof(int), "sec_cmpd", &sec_cmpd));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(grp_second, sec_cmpd, "x", 0, NC_INT));

  int leaf = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(grp_second, "leaf", &leaf));
  int leaf_var = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(leaf, "leaf_var", sec_cmpd, 1, &sec_dim, &leaf_var));
  int cross_var = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(leaf, "cross_var", root_enum, 1, &root_dim, &cross_var));

  ASSERT_EQ(NC_NOERR, nc.close());
}

TEST(PrintfqnTest, PrintsQualifiedTypeAndDimensionNamesAcrossGroups) {
  TempFile nc_file("printfqn_hierarchy.nc");
  ASSERT_NO_FATAL_FAILURE(CreateHierarchyFile(nc_file.c_str()));
  const std::string path = nc_file.string();

  struct QueryCase {
    const char* name;
    std::vector<std::string_view> args;
    std::string_view expected_stdout;
  };
  const QueryCase kCases[] = {
      {"root_type",
       {"printfqn", "-t", "-v", "root_var", "-f", path},
       "/root_enum"},
      {"root_dim",
       {"printfqn", "-d", "-v", "root_var", "-f", path},
       "/root_dim"},
      {"root_type_debug",
       {"printfqn", "-D", "-t", "-v", "root_var", "-f", path},
       "/root_enum"},
      {"first_group_type",
       {"printfqn", "-t", "-v", "first_var", "-f", path},
       "/grp_first/first_opaque"},
      {"first_group_dim",
       {"printfqn", "-d", "-v", "first_var", "-f", path},
       "/grp_first/first_dim"},
      {"nested_leaf_type",
       {"printfqn", "-t", "-v", "leaf_var", "-f", path},
       "/grp_second/sec_cmpd"},
      {"nested_leaf_dim",
       {"printfqn", "-d", "-v", "leaf_var", "-f", path},
       "/grp_second/sec_dim"},
      {"nested_var_root_type",
       {"printfqn", "-t", "-v", "cross_var", "-f", path},
       "/root_enum"},
      {"nested_var_root_dim",
       {"printfqn", "-d", "-v", "cross_var", "-f", path},
       "/root_dim"},
  };

  for (const QueryCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CliResult res = RunCliInForkedChild(printfqn_cli_main, tc.args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_EQ(tc.expected_stdout, res.stdout_output);
  }
}

TEST(PrintfqnTest, PrintsUsageOnVersionAndUnknownOptionFlags) {
  constexpr std::string_view kVersionArgs[] = {"printfqn", "-V"};
  const CliResult ver_res =
      RunCliInForkedChild(printfqn_cli_main, kVersionArgs);
  ASSERT_TRUE(ver_res.ok()) << ver_res.harness_error;
  EXPECT_EQ(0, ver_res.exit_code);
  EXPECT_THAT(ver_res.stderr_output, ::testing::HasSubstr("usage: printfqn"));

  constexpr std::string_view kUnknownArgs[] = {"printfqn", "-Z"};
  const CliResult unk_res =
      RunCliInForkedChild(printfqn_cli_main, kUnknownArgs);
  ASSERT_TRUE(unk_res.ok()) << unk_res.harness_error;
  EXPECT_EQ(0, unk_res.exit_code);
  EXPECT_THAT(unk_res.stderr_output, ::testing::HasSubstr("unknown option"));
  EXPECT_THAT(unk_res.stderr_output, ::testing::HasSubstr("usage: printfqn"));
}

TEST(PrintfqnTest, ExitsNonZeroForMissingFileMissingVariableOrAtomicType) {
  TempFile nc_file("printfqn_errors.nc");
  ASSERT_NO_FATAL_FAILURE(CreateHierarchyFile(nc_file.c_str()));
  const std::string path = nc_file.string();

  TempFile missing_file("printfqn_does_not_exist.nc");
  const std::string missing_path = missing_file.string();

  struct ErrorCase {
    const char* name;
    std::vector<std::string_view> args;
  };
  const ErrorCase kCases[] = {
      {"missing_file",
       {"printfqn", "-t", "-v", "root_var", "-f", missing_path}},
      {"missing_var", {"printfqn", "-t", "-v", "no_such_var", "-f", path}},
      {"atomic_type_not_in_typeids",
       {"printfqn", "-t", "-v", "root_atomic", "-f", path}},
  };

  for (const ErrorCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CliResult res = RunCliInForkedChild(printfqn_cli_main, tc.args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output, ::testing::HasSubstr("Error:"));
  }
}

}  // namespace
}  // namespace netcdf
