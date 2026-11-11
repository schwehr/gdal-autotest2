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

// Unit tests for //third_party/netcdf/ncgen/genlib.c.
//
// Covers topfqn, nestedfqn, attfqn, close_netcdf, and define_netcdf across C
// code generation and binary netCDF output modes.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/genlib.c

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
extern FILE* ncgin;
void freeSymbol(Symbol* sym);
}  // extern "C"

namespace netcdf {
namespace {

struct SymbolDeleter {
  void operator()(Symbol* sym) const { freeSymbol(sym); }
};
using ScopedSymbol = std::unique_ptr<Symbol, SymbolDeleter>;

ScopedSymbol MakeSymbol(nc_class obj_class, const char* name,
                        Symbol* container = nullptr) {
  auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
  sym->objectclass = obj_class;
  sym->name = estrdup(name);
  sym->container = container;
  return ScopedSymbol(sym);
}

void CleanupNcgenBuffers() {
  if (filldatalist != nullptr) {
    filldatalist->readonly = 0;
    freedatalist(filldatalist);
    filldatalist = nullptr;
  }
  bbFree(codebuffer);
  codebuffer = nullptr;
  bbFree(stmt);
  stmt = nullptr;
}

class GenlibTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CleanupNcgenBuffers();
    ncgen_shim_reset_globals();
  }

  void TearDown() override { CleanupNcgenBuffers(); }
};

TEST_F(GenlibTest, TopfqnNestedfqnAndAttfqnComputeAndCacheNames) {
  usingclassic = 0;

  ScopedSymbol root = MakeSymbol(NC_GRP, "root");
  root->grp.is_root = 1;
  topfqn(root.get());
  ASSERT_NE(nullptr, root->fqn);
  EXPECT_STREQ("", root->fqn);
  // Second call takes the already-defined fast path.
  topfqn(root.get());
  EXPECT_STREQ("", root->fqn);

  ScopedSymbol grp1 = MakeSymbol(NC_GRP, "grp.1", root.get());
  ScopedSymbol var1 = MakeSymbol(NC_VAR, "v/1", grp1.get());
  // Parent grp1->fqn is initially null, forcing recursive topfqn(parent).
  topfqn(var1.get());
  ASSERT_NE(nullptr, grp1->fqn);
  EXPECT_STREQ("/grp_DOT_1", grp1->fqn);
  ASSERT_NE(nullptr, var1->fqn);
  EXPECT_STREQ("/grp_DOT_1/v_SLASH_1", var1->fqn);

  ScopedSymbol var2 = MakeSymbol(NC_VAR, "v2", grp1.get());
  topfqn(var2.get());
  EXPECT_STREQ("/grp_DOT_1/v2", var2->fqn);

  usingclassic = 1;
  ScopedSymbol classic_var = MakeSymbol(NC_VAR, "classic_v", root.get());
  topfqn(classic_var.get());
  EXPECT_STREQ("classic_v", classic_var->fqn);

  ScopedSymbol type_sym = MakeSymbol(NC_TYPE, "mytype", root.get());
  type_sym->fqn = estrdup("/mytype");
  ScopedSymbol field_sym = MakeSymbol(NC_FIELD, "fld.1/a", type_sym.get());
  nestedfqn(field_sym.get());
  ASSERT_NE(nullptr, field_sym->fqn);
  EXPECT_STREQ("/mytype.fld_DOT_1_SLASH_a", field_sym->fqn);
  nestedfqn(field_sym.get());
  EXPECT_STREQ("/mytype.fld_DOT_1_SLASH_a", field_sym->fqn);

  ScopedSymbol global_att = MakeSymbol(NC_ATT, "g.att", nullptr);
  attfqn(global_att.get());
  ASSERT_NE(nullptr, global_att->fqn);
  EXPECT_STREQ("_g_DOT_att", global_att->fqn);
  attfqn(global_att.get());
  EXPECT_STREQ("_g_DOT_att", global_att->fqn);

  ScopedSymbol var_att = MakeSymbol(NC_ATT, "v/att", var1.get());
  attfqn(var_att.get());
  ASSERT_NE(nullptr, var_att->fqn);
  EXPECT_STREQ("/grp_DOT_1/v_SLASH_1_v_SLASH_att", var_att->fqn);
}

TEST_F(GenlibTest, CloseNetcdfHandlesUndefinedLanguageAsNoOp) {
  l_flag = L_UNDEFINED;
  close_netcdf();
  EXPECT_EQ(0, error_count);
}

TEST_F(GenlibTest, DefineNetcdfGeneratesCAndBinaryOutputs) {
  constexpr std::string_view kCdl =
      "netcdf genlib_ds {\n"
      "dimensions:\n"
      "  x = 2;\n"
      "variables:\n"
      "  int v(x);\n"
      "  v:units = \"m\";\n"
      "data:\n"
      "  v = 10, 20;\n"
      "}\n";

  TempFile cdl_file("genlib_input.cdl");
  {
    std::ofstream out(cdl_file.path(), std::ios::binary);
    out << kCdl;
  }

  TempFile c_out_file("genlib_generated.c");
  ASSERT_TRUE(RunInForkedChild([&]() -> bool {
    const int out_fd =
        ::open(c_out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out_fd < 0) {
      return false;
    }
    ::dup2(out_fd, STDOUT_FILENO);
    ::close(out_fd);

    ncgin = std::fopen(cdl_file.c_str(), "r");
    if (ncgin == nullptr) {
      return false;
    }
    l_flag = L_C;
    k_flag = NC_FORMAT_CLASSIC;
    usingclassic = 1;
    cmode_modifier = 0;

    parse_init();
    const int parse_stat = ncgparse();
    std::fclose(ncgin);
    ncgin = nullptr;
    if (parse_stat != 0) {
      return false;
    }
    processsemantics();
    define_netcdf();
    filldatalist = nullptr;
    std::fflush(stdout);
    return error_count == 0;
  }));

  {
    std::ifstream c_in(c_out_file.path(), std::ios::binary);
    const std::string c_src((std::istreambuf_iterator<char>(c_in)),
                            std::istreambuf_iterator<char>());
    EXPECT_THAT(c_src, ::testing::HasSubstr("nc_create"));
    EXPECT_THAT(c_src, ::testing::HasSubstr("nc_close"));
  }

  TempFile bin_out_file("genlib_output.nc");
  ASSERT_TRUE(RunInForkedChild([&]() -> bool {
    ncgin = std::fopen(cdl_file.c_str(), "r");
    if (ncgin == nullptr) {
      return false;
    }
    l_flag = L_BINARY;
    k_flag = NC_FORMAT_NETCDF4;
    usingclassic = 0;
    cmode_modifier = NC_NETCDF4;
    netcdf_name = estrdup(bin_out_file.c_str());

    parse_init();
    const int parse_stat = ncgparse();
    std::fclose(ncgin);
    ncgin = nullptr;
    if (parse_stat != 0) {
      return false;
    }
    processsemantics();
    define_netcdf();
    filldatalist = nullptr;
    return error_count == 0;
  }));

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_open(bin_out_file.c_str(), NC_NOWRITE, nc.ptr()));
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(nc.get(), "v", &varid));
  std::array<int, 2> vals{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(nc.get(), varid, vals.data()));
  EXPECT_THAT(vals, ::testing::ElementsAre(10, 20));
}

}  // namespace
}  // namespace netcdf
