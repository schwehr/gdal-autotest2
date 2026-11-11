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

// Unit tests for //third_party/netcdf/ncdump/utils.c.
//
// Exercises allocation wrappers, error/check exits, CDL escaping helpers,
// hierarchy/group/variable/dimension inquiry utilities, ID lists, option list
// parsers, group iterators, and single-string attribute extraction.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/utils.c

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
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
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

struct HierarchyIds {
  ScopedMemio memio;
  ScopedNcFile root;
  int g1 = -1;
  int g1_sub = -1;
  int g1_sub2 = -1;
  int g2 = -1;
  int g2_sub = -1;
  int g2_other = -1;
  int dim_root = -1;
  int dim_unlim = -1;
  int dim_g1 = -1;
  int var_scalar = -1;
  int var_fixed = -1;
  int var_rec0 = -1;
  int var_rec1 = -1;
  int var_g1_v1 = -1;
  int var_g1_sub_subv = -1;
  int var_g1_sub_leaf = -1;
  int var_g2_sub_subv = -1;
};

void BuildTestHierarchy(HierarchyIds& h) {
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("utils_tree.nc", NC_NETCDF4, 8192, nc.ptr()));

    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rd", 3, &h.dim_root));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "unlim", NC_UNLIMITED, &h.dim_unlim));

    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "sc", NC_INT, 0, nullptr, &h.var_scalar));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "rv", NC_INT, 1, &h.dim_root, &h.var_fixed));
    const int rec0_dims[2] = {h.dim_unlim, h.dim_root};
    const int rec1_dims[2] = {h.dim_root, h.dim_unlim};
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "rec0", NC_INT, 2, rec0_dims, &h.var_rec0));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "rec1", NC_INT, 2, rec1_dims, &h.var_rec1));

    constexpr char kCharAtt[] = "hello";
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(nc, h.var_scalar, "c_att", 5, kCharAtt));
    const char* kStr1[1] = {"world"};
    ASSERT_EQ(NC_NOERR,
              nc_put_att_string(nc, h.var_scalar, "s_att1", 1, kStr1));
    const char* kStr2[2] = {"first", "second"};
    ASSERT_EQ(NC_NOERR,
              nc_put_att_string(nc, h.var_scalar, "s_att2", 2, kStr2));
    constexpr int kIntAtt = 42;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc, h.var_scalar, "i_att", NC_INT, 1, &kIntAtt));

    int g1 = -1;
    int g1_sub = -1;
    int g1_sub2 = -1;
    int g2 = -1;
    int g2_sub = -1;
    int g2_other = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "g1", &g1));
    ASSERT_EQ(NC_NOERR, nc_def_dim(g1, "d1", 4, &h.dim_g1));
    int v1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(g1, "v1", NC_INT, 1, &h.dim_g1, &v1));

    ASSERT_EQ(NC_NOERR, nc_def_grp(g1, "sub", &g1_sub));
    int subv1 = -1;
    int leaf1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(g1_sub, "subv", NC_INT, 0, nullptr, &subv1));
    ASSERT_EQ(NC_NOERR, nc_def_var(g1_sub, "leaf", NC_INT, 0, nullptr, &leaf1));

    ASSERT_EQ(NC_NOERR, nc_def_grp(g1, "sub2", &g1_sub2));

    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "g2", &g2));
    ASSERT_EQ(NC_NOERR, nc_def_grp(g2, "sub", &g2_sub));
    int subv2 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(g2_sub, "subv", NC_INT, 0, nullptr, &subv2));
    ASSERT_EQ(NC_NOERR, nc_def_grp(g2, "other", &g2_other));

    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), h.memio.get()));
  }

  ASSERT_EQ(NC_NOERR, nc_open_mem("utils_tree.nc", NC_NOWRITE, h.memio.size(),
                                  h.memio.get()->memory, h.root.ptr()));
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(h.root, "g1", &h.g1));
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(h.g1, "sub", &h.g1_sub));
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(h.g1, "sub2", &h.g1_sub2));
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(h.root, "g2", &h.g2));
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(h.g2, "sub", &h.g2_sub));
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(h.g2, "other", &h.g2_other));

  ASSERT_EQ(NC_NOERR, nc_inq_dimid(h.root, "rd", &h.dim_root));
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(h.root, "unlim", &h.dim_unlim));
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(h.g1, "d1", &h.dim_g1));

  ASSERT_EQ(NC_NOERR, nc_inq_varid(h.root, "sc", &h.var_scalar));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(h.root, "rv", &h.var_fixed));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(h.root, "rec0", &h.var_rec0));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(h.root, "rec1", &h.var_rec1));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(h.g1, "v1", &h.var_g1_v1));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(h.g1_sub, "subv", &h.var_g1_sub_subv));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(h.g1_sub, "leaf", &h.var_g1_sub_leaf));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(h.g2_sub, "subv", &h.var_g2_sub_subv));
}

class UtilsTest : public ::testing::Test {
 protected:
  void SetUp() override { ncdump_stub_reset_globals(); }
  void TearDown() override { ncdump_stub_reset_globals(); }
};

TEST_F(UtilsTest, AllocWrappersHandleZeroAndPositiveSizes) {
  std::unique_ptr<void, decltype(&std::free)> p0(emalloc(0), &std::free);
  EXPECT_NE(nullptr, p0);

  std::unique_ptr<unsigned char, decltype(&std::free)> p1(
      static_cast<unsigned char*>(emalloc(16)), &std::free);
  ASSERT_NE(nullptr, p1);
  p1.get()[0] = 0xAB;

  std::unique_ptr<void, decltype(&std::free)> c0(ecalloc(0), &std::free);
  EXPECT_NE(nullptr, c0);

  std::unique_ptr<unsigned char, decltype(&std::free)> c1(
      static_cast<unsigned char*>(ecalloc(32)), &std::free);
  ASSERT_NE(nullptr, c1);
  for (int i = 0; i < 32; ++i) {
    EXPECT_EQ(0, c1.get()[i]);
  }

  void* raw = erealloc(nullptr, 8);
  ASSERT_NE(nullptr, raw);
  std::memset(raw, 0x5A, 8);
  std::unique_ptr<unsigned char, decltype(&std::free)> r2(
      static_cast<unsigned char*>(erealloc(raw, 64)), &std::free);
  ASSERT_NE(nullptr, r2);
  EXPECT_EQ(0x5A, r2.get()[0]);
  EXPECT_EQ(0x5A, r2.get()[7]);
}

TEST_F(UtilsTest, ErrorCheckAndReallocZeroExitInForkedChildren) {
  constexpr std::string_view kArgs[] = {"utils_exit_check"};

  {
    const CliResult res = RunCliInForkedChild(
        [](int /*argc*/, char** /*argv*/) -> int {
          char prog[] = "custom_prog";
          progname = prog;
          error("bad item %d", 7);
          return 0;
        },
        kArgs);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(EXIT_FAILURE, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("custom_prog: bad item 7\n"));
  }

  {
    const CliResult res = RunCliInForkedChild(
        [](int /*argc*/, char** /*argv*/) -> int {
          void* p = emalloc(8);
          void* next = erealloc(p, 0);
          std::free(next);
          return 0;
        },
        kArgs);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(EXIT_FAILURE, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("realloc with zero size"));
  }

  {
    const CliResult res = RunCliInForkedChild(
        [](int /*argc*/, char** /*argv*/) -> int {
          check(NC_EINVAL, "sample.c", "sample_fn", 123);
          return 0;
        },
        kArgs);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("Location: file sample.c"));
    EXPECT_THAT(res.stderr_output, HasSubstr("fcn sample_fn line 123"));
  }

  {
    const CliResult res = RunCliInForkedChild(
        [](int /*argc*/, char** /*argv*/) -> int {
          check(NC_EBADID, nullptr, nullptr, 9);
          return 0;
        },
        kArgs);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("file ?; fcn ? line 9"));
  }
}

TEST_F(UtilsTest, EscapedNameAndStringHandleSpecialCharsAndInvalidPrefix) {
  {
    const ScopedCString s = MakeScopedCString(escaped_name(""));
    ASSERT_NE(nullptr, s);
    EXPECT_STREQ("", s.get());
  }
  {
    const ScopedCString s = MakeScopedCString(escaped_name("0digit"));
    ASSERT_NE(nullptr, s);
    EXPECT_STREQ("\\0digit", s.get());
  }
  {
    const ScopedCString s = MakeScopedCString(escaped_name("9"));
    ASSERT_NE(nullptr, s);
    EXPECT_STREQ("\\9", s.get());
  }
  {
    const ScopedCString s =
        MakeScopedCString(escaped_name("a\x01\n\x7f"
                                       "b"));
    ASSERT_NE(nullptr, s);
    EXPECT_STREQ(R"(a\%01\%0a\%7fb)", s.get());
  }
  {
    const ScopedCString s =
        MakeScopedCString(escaped_name(R"(a !"#$&'()*,:;<=>?[\]^`{|}~b)"));
    ASSERT_NE(nullptr, s);
    EXPECT_STREQ(R"(a\ \!\"\#\$\&\'\(\)\*\,\:\;\<\=\>\?\[\\\]\^\`\{\|\}\~b)",
                 s.get());
  }
  {
    const ScopedCString s =
        MakeScopedCString(escaped_name("dir/sub_1-2.3%z\xc3\xa9"));
    ASSERT_NE(nullptr, s);
    EXPECT_STREQ("dir/sub_1-2.3%z\xc3\xa9", s.get());
  }

  // print_name writes escaped_name to stdout.
  {
    constexpr std::string_view kArgs[] = {"print_name_test"};
    const CliResult res = RunCliInForkedChild(
        [](int /*argc*/, char** /*argv*/) -> int {
          print_name("a b/c");
          return 0;
        },
        kArgs);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code);
    EXPECT_EQ("a\\ b/c", res.stdout_output);
  }

  // Leading space, control char, or DEL rejects via error().
  for (const char* bad : {" leading", "\x01ctrl",
                          "\x7f"
                          "del"}) {
    SCOPED_TRACE(bad);
    constexpr std::string_view kArgs[] = {"bad_escaped_name"};
    const CliResult res = RunCliInForkedChild(
        [bad](int /*argc*/, char** /*argv*/) -> int {
          char* out = escaped_name(bad);
          std::free(out);
          return 0;
        },
        kArgs);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(EXIT_FAILURE, res.exit_code);
    EXPECT_THAT(res.stderr_output,
                HasSubstr("name begins with space or control-character"));
  }

  // escaped_string.
  {
    const ScopedCString s = MakeScopedCString(escaped_string(""));
    EXPECT_STREQ("", s.get());
  }
  {
    const ScopedCString s =
        MakeScopedCString(escaped_string("x\n\t\x1f\"q\"\\b \xc3\xa9"));
    EXPECT_STREQ(R"(x\%0a\%09\%1f\"q\"\b )"
                 "\xc3\xa9",
                 s.get());
  }
}

TEST_F(UtilsTest, GroupVarDimAndRecordQueriesWorkAcrossHierarchy) {
  HierarchyIds h;
  BuildTestHierarchy(h);

  // getrootid.
  EXPECT_EQ(h.root.get(), getrootid(h.root));
  EXPECT_EQ(h.root.get(), getrootid(h.g1));
  EXPECT_EQ(h.root.get(), getrootid(h.g1_sub));

  // nc_inq_grpid2.
  int gid = -1;
  EXPECT_EQ(NC_NOERR, nc_inq_grpid2(h.root, "g1", &gid));
  EXPECT_EQ(h.g1, gid);
  EXPECT_EQ(NC_NOERR, nc_inq_grpid2(h.g1_sub, "/", &gid));
  EXPECT_EQ(h.root.get(), gid);
  EXPECT_EQ(NC_NOERR, nc_inq_grpid2(h.g2, "/g1/sub", &gid));
  EXPECT_EQ(h.g1_sub, gid);
  EXPECT_EQ(NC_NOERR, nc_inq_grpid2(h.root, "g1/sub", &gid));
  EXPECT_EQ(h.g1_sub, gid);
  EXPECT_EQ(NC_NOERR, nc_inq_grpid2(h.root, "/g1/sub", nullptr));
  EXPECT_EQ(NC_ENOGRP, nc_inq_grpid2(h.root, "/g1/nope", &gid));

  // nc_inq_varid2.
  int vid = -1;
  gid = -1;
  EXPECT_EQ(NC_NOERR, nc_inq_varid2(h.root, "sc", &vid, &gid));
  EXPECT_EQ(h.var_scalar, vid);
  EXPECT_EQ(h.root.get(), gid);

  EXPECT_EQ(NC_NOERR, nc_inq_varid2(h.g2, "/g1/sub/subv", &vid, &gid));
  EXPECT_EQ(h.var_g1_sub_subv, vid);
  EXPECT_EQ(h.g1_sub, gid);

  EXPECT_EQ(NC_NOERR, nc_inq_varid2(h.root, "g1/v1", &vid, &gid));
  EXPECT_EQ(h.var_g1_v1, vid);
  EXPECT_EQ(h.g1, gid);

  EXPECT_EQ(NC_NOERR, nc_inq_varid2(h.root, "/g1/v1", nullptr, nullptr));
  EXPECT_EQ(NC_ENOGRP, nc_inq_varid2(h.root, "nope/v1", &vid, &gid));
  EXPECT_EQ(NC_ENOTVAR, nc_inq_varid2(h.root, "/g1/nope", &vid, &gid));

  // nc_inq_dimid2.
  int did = -1;
  EXPECT_EQ(NC_NOERR, nc_inq_dimid2(h.root, "rd", &did));
  EXPECT_EQ(h.dim_root, did);
  EXPECT_EQ(NC_EBADDIM, nc_inq_dimid2(h.root, "missing_dim", &did));
  did = -1;
  EXPECT_EQ(NC_NOERR, nc_inq_dimid2(h.root, "/g1/d1", &did));
  EXPECT_EQ(h.dim_g1, did);
  EXPECT_EQ(NC_EINVAL, nc_inq_dimid2(h.root, "g1/d1", &did));
  EXPECT_EQ(NC_ENOGRP, nc_inq_dimid2(h.root, "/missing/d1", &did));

  // isrecvar.
  EXPECT_EQ(0, isrecvar(h.root, h.var_scalar));
  EXPECT_EQ(0, isrecvar(h.root, h.var_fixed));
  EXPECT_EQ(1, isrecvar(h.root, h.var_rec0));
  EXPECT_EQ(1, isrecvar(h.root, h.var_rec1));

  // strendswith.
  EXPECT_EQ(0, strendswith("a", "ab"));
  EXPECT_EQ(1, strendswith("/g1/sub", "g1/sub"));
  EXPECT_EQ(0, strendswith("/g2/sub", "g1/sub"));
  EXPECT_EQ(1, strendswith("abc", ""));
  EXPECT_EQ(1, strendswith("", ""));

  // nc_inq_gvarid.
  EXPECT_EQ(NC_ENOTVAR, nc_inq_gvarid(h.root, "", &vid));
  EXPECT_EQ(NC_NOERR, nc_inq_gvarid(h.g1_sub, "subv", &vid));
  EXPECT_EQ(h.var_g1_sub_subv, vid);
  EXPECT_EQ(NC_ENOTVAR, nc_inq_gvarid(h.g1_sub, "missing", &vid));
  EXPECT_EQ(NC_NOERR, nc_inq_gvarid(h.g1_sub, "/g1/sub/subv", &vid));
  EXPECT_EQ(h.var_g1_sub_subv, vid);
  EXPECT_EQ(NC_ENOTVAR, nc_inq_gvarid(h.g2_sub, "/g1/sub/subv", &vid));
  EXPECT_EQ(NC_ENOTVAR, nc_inq_gvarid(h.root, "/sc", &vid));
  EXPECT_EQ(NC_NOERR, nc_inq_gvarid(h.g1_sub, "g1/sub/subv", &vid));
  EXPECT_EQ(h.var_g1_sub_subv, vid);
  EXPECT_EQ(NC_ENOTVAR, nc_inq_gvarid(h.g2_sub, "g1/sub/subv", &vid));
  EXPECT_EQ(NC_EBADID, nc_inq_gvarid(-1, "a/b", &vid));

  // nc_inq_varname_count and missing_vars.
  char var_rel[] = "subv";
  char var_abs[] = "/g1/sub/subv";
  char var_part[] = "g2/sub/subv";
  char var_none[] = "no_such_var";
  EXPECT_EQ(2u, nc_inq_varname_count(h.root, var_rel));
  EXPECT_EQ(1u, nc_inq_varname_count(h.root, var_abs));
  EXPECT_EQ(1u, nc_inq_varname_count(h.root, var_part));
  EXPECT_EQ(0u, nc_inq_varname_count(h.root, var_none));

  EXPECT_EQ(0, missing_vars(h.root, 0, nullptr));
  char v_rv[] = "rv";
  char* ok_vars[2] = {v_rv, var_rel};
  EXPECT_EQ(0, missing_vars(h.root, 2, ok_vars));
}

TEST_F(UtilsTest, IdListGroupMatchesMakeLvarsMakeLgrpsAndErrors) {
  // idnode_t + group_wanted.
  idnode_t* list = newidlist();
  ASSERT_NE(nullptr, list);
  absl::Cleanup free_list = [&] { freeidlist(list); };

  EXPECT_EQ(0, idmember(list, 10));
  idadd(list, 10);
  idadd(list, 20);
  EXPECT_EQ(1, idmember(list, 10));
  EXPECT_EQ(1, idmember(list, 20));
  EXPECT_EQ(0, idmember(list, 99));

  EXPECT_EQ(1, group_wanted(999, 0, nullptr));
  EXPECT_EQ(1, group_wanted(10, 2, list));
  EXPECT_EQ(0, group_wanted(999, 2, list));
  freeidlist(nullptr);

  // make_lvars & make_lgrps.
  {
    std::string var_arg = "alpha,beta,/g1/gamma";
    int nlvars = 0;
    char** lvars = nullptr;
    make_lvars(var_arg.data(), &nlvars, &lvars);
    absl::Cleanup free_lvars = [&] {
      for (int i = 0; i < nlvars; ++i) {
        std::free(lvars[i]);
      }
      std::free(lvars);
    };
    EXPECT_EQ(3, nlvars);
    EXPECT_STREQ("alpha", lvars[0]);
    EXPECT_STREQ("beta", lvars[1]);
    EXPECT_STREQ("/g1/gamma", lvars[2]);
  }
  {
    std::string grp_arg = "/,g1,sub";
    int nlgrps = 0;
    char** lgrps = nullptr;
    idnode_t* grpids = nullptr;
    make_lgrps(grp_arg.data(), &nlgrps, &lgrps, &grpids);
    absl::Cleanup free_lgrps = [&] {
      for (int i = 0; i < nlgrps; ++i) {
        std::free(lgrps[i]);
      }
      std::free(lgrps);
      freeidlist(grpids);
    };
    EXPECT_EQ(3, nlgrps);
    EXPECT_STREQ("/", lgrps[0]);
    EXPECT_STREQ("g1", lgrps[1]);
    EXPECT_STREQ("sub", lgrps[2]);
    ASSERT_NE(nullptr, grpids);
  }

  // grp_matches against test hierarchy.
  HierarchyIds h;
  BuildTestHierarchy(h);
  {
    idnode_t* grpids = newidlist();
    absl::Cleanup free_grpids = [&] { freeidlist(grpids); };
    EXPECT_EQ(0u, grp_matches(h.root, 0, nullptr, grpids));

    char g_empty[] = "";
    char g_slash[] = "/";
    char g_abs[] = "/g1/sub";
    char g_rel[] = "sub";
    char* lgrps[4] = {g_empty, g_slash, g_abs, g_rel};
    EXPECT_EQ(5u, grp_matches(h.root, 4, lgrps, grpids));
    EXPECT_EQ(1, idmember(grpids, h.root));
    EXPECT_EQ(1, idmember(grpids, h.g1_sub));
    EXPECT_EQ(1, idmember(grpids, h.g2_sub));
    EXPECT_EQ(0, idmember(grpids, h.g2_other));
  }

  // missing_vars and grp_matches fatal error paths inside forked children.
  {
    constexpr std::string_view kArgs[] = {"missing_var_err"};
    const CliResult res = RunCliInForkedChild(
        [](int /*argc*/, char** /*argv*/) -> int {
          ScopedNcFile nc;
          if (nc_create_mem("mv_err.nc", NC_NETCDF4, 4096, nc.ptr()) !=
              NC_NOERR) {
            return 2;
          }
          int varid = -1;
          if (nc_def_var(nc, "present", NC_INT, 0, nullptr, &varid) !=
              NC_NOERR) {
            return 2;
          }
          if (nc_enddef(nc) != NC_NOERR) {
            return 2;
          }
          char bad_var[] = "does_not_exist";
          char* lvars[1] = {bad_var};
          missing_vars(nc, 1, lvars);
          return 0;
        },
        kArgs);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(EXIT_FAILURE, res.exit_code);
    EXPECT_THAT(res.stderr_output,
                HasSubstr("does_not_exist: No such variable"));
  }

  for (const char* bad_grp : {"/g1/missing", "missing_rel"}) {
    SCOPED_TRACE(bad_grp);
    constexpr std::string_view kArgs[] = {"missing_grp_err"};
    const CliResult res = RunCliInForkedChild(
        [bad_grp](int /*argc*/, char** /*argv*/) -> int {
          ScopedNcFile nc;
          if (nc_create_mem("mg_err.nc", NC_NETCDF4, 4096, nc.ptr()) !=
              NC_NOERR) {
            return 2;
          }
          int g1 = -1;
          if (nc_def_grp(nc, "g1", &g1) != NC_NOERR) {
            return 2;
          }
          if (nc_enddef(nc) != NC_NOERR) {
            return 2;
          }
          std::string buf(bad_grp);
          char* lgrps[1] = {buf.data()};
          idnode_t* grpids = newidlist();
          grp_matches(nc, 1, lgrps, grpids);
          freeidlist(grpids);
          return 0;
        },
        kArgs);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(EXIT_FAILURE, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("No such group"));
  }

  {
    constexpr std::string_view kArgs[] = {"bad_ncid_grp_err"};
    const CliResult res = RunCliInForkedChild(
        [](int /*argc*/, char** /*argv*/) -> int {
          char abs_grp[] = "/g1";
          char* lgrps[1] = {abs_grp};
          idnode_t* grpids = newidlist();
          grp_matches(-1, 1, lgrps, grpids);
          freeidlist(grpids);
          return 0;
        },
        kArgs);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(EXIT_FAILURE, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("when looking up group /g1"));
  }
}

TEST_F(UtilsTest, GroupIteratorAndFullInqTraversePreorderAndFreeMidway) {
  HierarchyIds h;
  BuildTestHierarchy(h);

  // Invalid grpid.
  ncgiter_t* bad_iter = nullptr;
  EXPECT_EQ(NC_EBADID, nc_get_giter(-1, &bad_iter));

  // nc_inq_grps_full with numgrps, then with nullptr numgrps.
  size_t count = 0;
  EXPECT_EQ(NC_NOERR, nc_inq_grps_full(h.root, &count, nullptr));
  ASSERT_EQ(7u, count);
  std::vector<int> ids(count, -1);
  EXPECT_EQ(NC_NOERR, nc_inq_grps_full(h.root, nullptr, ids.data()));
  EXPECT_THAT(ids, ElementsAre(h.root.get(), h.g1, h.g1_sub, h.g1_sub2, h.g2,
                               h.g2_sub, h.g2_other));

  // Manual full iteration + repeated calls after exhaustion.
  {
    ncgiter_t* iter = nullptr;
    ASSERT_EQ(NC_NOERR, nc_get_giter(h.root, &iter));
    ASSERT_NE(nullptr, iter);
    absl::Cleanup free_iter = [&] { nc_free_giter(iter); };

    std::vector<int> visited;
    int grpid = -1;
    ASSERT_EQ(NC_NOERR, nc_next_giter(iter, &grpid));
    while (grpid != 0) {
      visited.push_back(grpid);
      ASSERT_EQ(NC_NOERR, nc_next_giter(iter, &grpid));
    }
    EXPECT_THAT(visited, ElementsAre(h.root.get(), h.g1, h.g1_sub, h.g1_sub2,
                                     h.g2, h.g2_sub, h.g2_other));

    // Calling nc_next_giter again on an empty iterator still yields 0.
    grpid = -1;
    EXPECT_EQ(NC_NOERR, nc_next_giter(iter, &grpid));
    EXPECT_EQ(0, grpid);
  }

  // Partial iteration leaves stacked subgroups so nc_free_giter frees nodes.
  {
    ncgiter_t* iter = nullptr;
    ASSERT_EQ(NC_NOERR, nc_get_giter(h.root, &iter));
    ASSERT_NE(nullptr, iter);
    absl::Cleanup free_iter = [&] { nc_free_giter(iter); };
    int first = -1;
    int second = -1;
    ASSERT_EQ(NC_NOERR, nc_next_giter(iter, &first));
    ASSERT_EQ(NC_NOERR, nc_next_giter(iter, &second));
    EXPECT_EQ(h.root.get(), first);
    EXPECT_EQ(h.g1, second);
  }

  // nc_inq_grps_full on invalid root hits NC_CHECK -> exit(1).
  {
    constexpr std::string_view kArgs[] = {"grps_full_bad_id"};
    const CliResult res = RunCliInForkedChild(
        [](int /*argc*/, char** /*argv*/) -> int {
          size_t n = 0;
          nc_inq_grps_full(-1, &n, nullptr);
          return 0;
        },
        kArgs);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
  }
}

TEST_F(UtilsTest, GetAttSingleStringHandlesCharStringAndInvalidType) {
  HierarchyIds h;
  BuildTestHierarchy(h);

  // NC_CHAR attribute.
  {
    nctype_t tinfo{};
    tinfo.size = sizeof(char);
    ncatt_t att{};
    std::snprintf(att.name, sizeof(att.name), "c_att");
    att.type = NC_CHAR;
    att.tinfo = &tinfo;
    att.len = 5;

    char* raw_out = nullptr;
    nc_get_att_single_string(h.root, h.var_scalar, &att, &raw_out);
    const ScopedCString out = MakeScopedCString(raw_out);
    ASSERT_NE(nullptr, out);
    EXPECT_STREQ("hello", out.get());
  }

  // NC_STRING attribute (1 element).
  {
    nctype_t tinfo{};
    tinfo.size = sizeof(char*);
    ncatt_t att{};
    std::snprintf(att.name, sizeof(att.name), "s_att1");
    att.type = NC_STRING;
    att.tinfo = &tinfo;
    att.len = 1;

    char* raw_out = nullptr;
    nc_get_att_single_string(h.root, h.var_scalar, &att, &raw_out);
    const ScopedCString out = MakeScopedCString(raw_out);
    ASSERT_NE(nullptr, out);
    EXPECT_STREQ("world", out.get());
  }

  // NC_STRING attribute (2 elements: uses first, cleans up all).
  {
    nctype_t tinfo{};
    tinfo.size = sizeof(char*);
    ncatt_t att{};
    std::snprintf(att.name, sizeof(att.name), "s_att2");
    att.type = NC_STRING;
    att.tinfo = &tinfo;
    att.len = 2;

    char* raw_out = nullptr;
    nc_get_att_single_string(h.root, h.var_scalar, &att, &raw_out);
    const ScopedCString out = MakeScopedCString(raw_out);
    ASSERT_NE(nullptr, out);
    EXPECT_STREQ("first", out.get());
  }

  // Invalid attribute type exits with code 2.
  {
    constexpr std::string_view kArgs[] = {"att_single_str_bad_type"};
    const CliResult res = RunCliInForkedChild(
        [](int /*argc*/, char** /*argv*/) -> int {
          ScopedNcFile nc;
          if (nc_create_mem("att_bad.nc", NC_NETCDF4, 4096, nc.ptr()) !=
              NC_NOERR) {
            return 1;
          }
          int varid = -1;
          if (nc_def_var(nc, "sc", NC_INT, 0, nullptr, &varid) != NC_NOERR) {
            return 1;
          }
          constexpr int kVal = 42;
          if (nc_put_att_int(nc, varid, "i_att", NC_INT, 1, &kVal) !=
              NC_NOERR) {
            return 1;
          }
          if (nc_enddef(nc) != NC_NOERR) {
            return 1;
          }
          nctype_t tinfo{};
          tinfo.size = sizeof(int);
          ncatt_t att{};
          std::snprintf(att.name, sizeof(att.name), "i_att");
          att.type = NC_INT;
          att.tinfo = &tinfo;
          att.len = 1;
          char* raw_out = nullptr;
          nc_get_att_single_string(nc, varid, &att, &raw_out);
          std::free(raw_out);
          return 0;
        },
        kArgs);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(2, res.exit_code);
    EXPECT_THAT(res.stderr_output,
                HasSubstr("nc_get_att_single_string: unknown attribute type"));
  }
}

}  // namespace
}  // namespace netcdf
