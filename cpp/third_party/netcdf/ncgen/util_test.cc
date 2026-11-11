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

// Unit tests for //third_party/netcdf/ncgen/util.c.
//
// Covers string/hex/float/byte utilities (append, chartohex, expe2d, pow2,
// tztrim, makebytestring, getpadding), symbol reclamation (freeSymbol,
// cleanup), type/class/dimset helpers, prefix/pool utilities,
// check_err/check_err2, and kind_string.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/util.c

#include <fcntl.h>
#include <setjmp.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
char* append(const char* s1, const char* s2);
void freeSymbol(Symbol* sym);
char* prefixtostring(List* prefix, char* separator);
size_t prefixarraylength(Dimset* dimset, int last);
}  // extern "C"

namespace netcdf {
namespace {

struct SymbolDeleter {
  void operator()(Symbol* sym) const { freeSymbol(sym); }
};
using ScopedSymbol = std::unique_ptr<Symbol, SymbolDeleter>;

Symbol* MakeRawSymbol(nc_class obj_class, const char* name,
                      Symbol* container = nullptr) {
  auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
  sym->objectclass = obj_class;
  sym->name = estrdup(name);
  sym->container = container;
  return sym;
}

ScopedSymbol MakeScopedSymbol(nc_class obj_class, const char* name,
                              Symbol* container = nullptr) {
  return ScopedSymbol(MakeRawSymbol(obj_class, name, container));
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

class UtilTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CleanupNcgenBuffers();
    ncgen_shim_reset_globals();
  }

  void TearDown() override {
    ncgen_shim_set_finalize_jmp(nullptr, nullptr);
    CleanupNcgenBuffers();
  }
};

template <typename Fn>
std::string CaptureStderr(Fn&& fn) {
  TempFile err_file("util_stderr.txt");
  std::fflush(stderr);
  const int saved_fd = ::dup(STDERR_FILENO);
  if (saved_fd < 0) {
    return "";
  }
  const int tmp_fd =
      ::open(err_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (tmp_fd < 0) {
    ::close(saved_fd);
    return "";
  }
  ::dup2(tmp_fd, STDERR_FILENO);
  ::close(tmp_fd);

  {
    const absl::Cleanup restore_stderr = [saved_fd] {
      std::fflush(stderr);
      ::dup2(saved_fd, STDERR_FILENO);
      ::close(saved_fd);
    };
    fn();
  }

  std::ifstream in(err_file.path(), std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

TEST_F(UtilTest, AppendExpe2dPow2ChartohexAndTztrim) {
  using ScopedCString = std::unique_ptr<char, decltype(&std::free)>;

  ScopedCString both_null(append(nullptr, nullptr), &std::free);
  EXPECT_STREQ("", both_null.get());
  ScopedCString left_only(append("abc", nullptr), &std::free);
  EXPECT_STREQ("abc", left_only.get());
  ScopedCString right_only(append(nullptr, "xyz"), &std::free);
  EXPECT_STREQ("xyz", right_only.get());
  ScopedCString both(append("abc", "xyz"), &std::free);
  EXPECT_STREQ("abcxyz", both.get());

  for (char c = '0'; c <= '9'; ++c) {
    EXPECT_EQ(static_cast<unsigned int>(c - '0'), chartohex(c));
  }
  for (char c = 'A'; c <= 'F'; ++c) {
    EXPECT_EQ(static_cast<unsigned int>(c - 'A' + 10), chartohex(c));
  }
  for (char c = 'a'; c <= 'f'; ++c) {
    EXPECT_EQ(static_cast<unsigned int>(c - 'a' + 10), chartohex(c));
  }
  EXPECT_EQ(0U, chartohex('G'));
  EXPECT_EQ(0U, chartohex('z'));
  EXPECT_EQ(0U, chartohex('/'));

  char exp1[] = "1.5e-3";
  expe2d(exp1);
  EXPECT_STREQ("1.5d-3", exp1);
  char exp2[] = "1e2e3";
  expe2d(exp2);
  EXPECT_STREQ("1e2d3", exp2);
  char exp_none[] = "1.5d-3";
  expe2d(exp_none);
  EXPECT_STREQ("1.5d-3", exp_none);

  EXPECT_EQ(0, pow2(0));
  EXPECT_EQ(0, pow2(-4));
  EXPECT_NE(0, pow2(1));
  EXPECT_NE(0, pow2(2));
  EXPECT_NE(0, pow2(16));
  EXPECT_NE(0, pow2(1024));
  EXPECT_EQ(0, pow2(3));
  EXPECT_EQ(0, pow2(6));

  char tz1[] = "-1.2300";
  tztrim(tz1);
  EXPECT_STREQ("-1.23", tz1);
  char tz2[] = "1.2000e+5";
  tztrim(tz2);
  EXPECT_STREQ("1.2e+5", tz2);
  char tz3[] = "12.";
  tztrim(tz3);
  EXPECT_STREQ("12.", tz3);
  char tz4[] = "1.23";
  tztrim(tz4);
  EXPECT_STREQ("1.23", tz4);
  char tz5[] = "-1.5e2";
  tztrim(tz5);
  EXPECT_STREQ("-1.5e2", tz5);
  char tz6[] = "1.000";
  tztrim(tz6);
  EXPECT_STREQ("1.", tz6);
}

TEST_F(UtilTest, FreeSymbolHandlesNullAndAllObjectClassesCleanly) {
  freeSymbol(nullptr);

  ScopedSymbol full_var = MakeScopedSymbol(NC_VAR, "full_var");
  full_var->fqn = estrdup("/full_var");
  full_var->prefix = listnew();
  full_var->subnodes = listnew();
  full_var->data = builddatalist(1);
  dlappend(full_var->data, nullconst());
  full_var->var.attributes = listnew();
  full_var->var.special._Fillvalue = builddatalist(1);
  dlappend(full_var->var.special._Fillvalue, nullconst());
  full_var->var.special.nchunks = 2;
  full_var->var.special._ChunkSizes =
      static_cast<size_t*>(ecalloc(sizeof(size_t) * 2));
  ASSERT_EQ(NC_NOERR, ncaux_h5filterspec_parselist(
                          "32001,1", nullptr, &full_var->var.special.nfilters,
                          &full_var->var.special._Filters));
  full_var->var.special._Codecs = estrdup("[]");
  full_var.reset();

  ScopedSymbol empty_var = MakeScopedSymbol(NC_VAR, "empty_var");
  empty_var.reset();

  ScopedSymbol full_type = MakeScopedSymbol(NC_TYPE, "full_type");
  full_type->typ.econst = nullconst();
  full_type->typ._Fillvalue = builddatalist(1);
  dlappend(full_type->typ._Fillvalue, nullconst());
  full_type.reset();

  ScopedSymbol empty_type = MakeScopedSymbol(NC_TYPE, "empty_type");
  empty_type.reset();

  ScopedSymbol grp_with_file = MakeScopedSymbol(NC_GRP, "grp_file");
  grp_with_file->file.filename = estrdup("out.nc");
  grp_with_file.reset();

  ScopedSymbol grp_no_file = MakeScopedSymbol(NC_GRP, "grp_plain");
  grp_no_file.reset();

  ScopedSymbol dim_sym = MakeScopedSymbol(NC_DIM, "dim_x");
  dim_sym.reset();
}

TEST_F(UtilTest, TypeClassAndSizeNamesMatchExpectedValues) {
  constexpr nc_type kStdTypes[] = {
      NC_NAT,    NC_BYTE,  NC_CHAR,   NC_SHORT, NC_INT,      NC_FLOAT,
      NC_DOUBLE, NC_UBYTE, NC_USHORT, NC_UINT,  NC_INT64,    NC_UINT64,
      NC_STRING, NC_VLEN,  NC_OPAQUE, NC_ENUM,  NC_COMPOUND,
  };
  constexpr const char* kStdNames[] = {
      "NC_NAT",   "NC_BYTE",     "NC_CHAR",   "NC_SHORT",  "NC_INT",
      "NC_FLOAT", "NC_DOUBLE",   "NC_UBYTE",  "NC_USHORT", "NC_UINT",
      "NC_INT64", "NC_UINT64",   "NC_STRING", "NC_VLEN",   "NC_OPAQUE",
      "NC_ENUM",  "NC_COMPOUND",
  };
  constexpr size_t kStdSizes[] = {
      0, 1, 1, 2, 4, 4, 8, 1, 2, 4, 8, 8, sizeof(char*), sizeof(nc_vlen_t),
      0, 0, 0,
  };
  for (size_t i = 0; i < std::size(kStdTypes); ++i) {
    EXPECT_STREQ(kStdNames[i], nctypename(kStdTypes[i]));
    EXPECT_STREQ(kStdNames[i], ncclassname(kStdTypes[i]));
    EXPECT_EQ(kStdSizes[i], ncsize(kStdTypes[i]));
  }

  EXPECT_STREQ("NC_GRP", nctypename(NC_GRP));
  EXPECT_STREQ("NC_TYPE", nctypename(NC_TYPE));
  EXPECT_STREQ("NC_PRIM", nctypename(NC_PRIM));
  EXPECT_STREQ("NC_FILL", nctypename(NC_FILLVALUE));
  EXPECT_STREQ("NC_NIL", nctypename(NC_NIL));
  EXPECT_STREQ("NC_<-5>", nctypename(-5));
  EXPECT_STREQ("NC_<999>", nctypename(999));

  EXPECT_STREQ("NC_FILL", ncclassname(NC_FILLVALUE));
  EXPECT_STREQ("NC_GRP", ncclassname(NC_GRP));
  EXPECT_STREQ("NC_TYP", ncclassname(NC_TYPE));
  EXPECT_STREQ("NC_PRIM", ncclassname(NC_PRIM));
  EXPECT_STREQ("NC_<32>", ncclassname(NC_NIL));
  EXPECT_STREQ("NC_<999>", ncclassname(999));

  EXPECT_EQ(0U, ncsize(-1));
  EXPECT_EQ(0U, ncsize(NC_COMPOUND + 1));
}

TEST_F(UtilTest, DimsetPredicatesAndCountsHandleBoundedAndUnlimitedDims) {
  ScopedSymbol d3 = MakeScopedSymbol(NC_DIM, "d3");
  d3->dim.declsize = 3;
  d3->dim.isunlimited = 0;
  ScopedSymbol d4 = MakeScopedSymbol(NC_DIM, "d4");
  d4->dim.declsize = 4;
  d4->dim.isunlimited = 0;
  ScopedSymbol d5 = MakeScopedSymbol(NC_DIM, "d5");
  d5->dim.declsize = 5;
  d5->dim.isunlimited = 0;
  ScopedSymbol du = MakeScopedSymbol(NC_DIM, "du");
  du->dim.declsize = NC_UNLIMITED;
  du->dim.isunlimited = 1;

  Dimset empty{};
  empty.ndims = 0;
  EXPECT_EQ(0, hasunlimited(&empty));
  EXPECT_EQ(0, isunlimited0(&empty));
  EXPECT_EQ(1, classicunlimited(&empty));
  EXPECT_EQ(1, isbounded(&empty));
  EXPECT_EQ(0, findunlimited(&empty, 0));
  EXPECT_EQ(0, findlastunlimited(&empty));
  EXPECT_EQ(0, countunlimited(&empty));

  Dimset bounded{};
  bounded.ndims = 3;
  bounded.dimsyms[0] = d3.get();
  bounded.dimsyms[1] = d4.get();
  bounded.dimsyms[2] = d5.get();
  EXPECT_EQ(0, hasunlimited(&bounded));
  EXPECT_EQ(0, isunlimited0(&bounded));
  EXPECT_EQ(1, classicunlimited(&bounded));
  EXPECT_EQ(1, isbounded(&bounded));
  EXPECT_EQ(60U, crossproduct(&bounded, 0, 3));
  EXPECT_EQ(20U, crossproduct(&bounded, 1, 3));
  EXPECT_EQ(12U, prefixarraylength(&bounded, 1));
  EXPECT_EQ(3, findunlimited(&bounded, 0));
  EXPECT_EQ(3, findlastunlimited(&bounded));
  EXPECT_EQ(0, countunlimited(&bounded));

  Dimset unlim_first{};
  unlim_first.ndims = 2;
  unlim_first.dimsyms[0] = du.get();
  unlim_first.dimsyms[1] = d5.get();
  EXPECT_EQ(1, hasunlimited(&unlim_first));
  EXPECT_EQ(1, isunlimited0(&unlim_first));
  EXPECT_EQ(1, classicunlimited(&unlim_first));
  EXPECT_EQ(0, isbounded(&unlim_first));
  EXPECT_EQ(0, findunlimited(&unlim_first, 0));
  EXPECT_EQ(2, findunlimited(&unlim_first, 1));
  EXPECT_EQ(0, findlastunlimited(&unlim_first));
  EXPECT_EQ(1, countunlimited(&unlim_first));

  Dimset unlim_second{};
  unlim_second.ndims = 2;
  unlim_second.dimsyms[0] = d5.get();
  unlim_second.dimsyms[1] = du.get();
  EXPECT_EQ(1, hasunlimited(&unlim_second));
  EXPECT_EQ(0, isunlimited0(&unlim_second));
  EXPECT_EQ(0, classicunlimited(&unlim_second));
  EXPECT_EQ(0, isbounded(&unlim_second));
  EXPECT_EQ(1, findunlimited(&unlim_second, 0));
  EXPECT_EQ(1, findlastunlimited(&unlim_second));
  EXPECT_EQ(1, countunlimited(&unlim_second));

  Dimset multi_unlim{};
  multi_unlim.ndims = 3;
  multi_unlim.dimsyms[0] = du.get();
  multi_unlim.dimsyms[1] = d4.get();
  multi_unlim.dimsyms[2] = du.get();
  EXPECT_EQ(0, classicunlimited(&multi_unlim));
  EXPECT_EQ(0, findunlimited(&multi_unlim, 0));
  EXPECT_EQ(2, findunlimited(&multi_unlim, 1));
  EXPECT_EQ(2, findlastunlimited(&multi_unlim));
  EXPECT_EQ(2, countunlimited(&multi_unlim));
}

TEST_F(UtilTest, TypeClassificationAndSignConversionsWork) {
  EXPECT_EQ(NC_BYTE, signedtype(NC_BYTE));
  EXPECT_EQ(NC_BYTE, signedtype(NC_UBYTE));
  EXPECT_EQ(NC_SHORT, signedtype(NC_SHORT));
  EXPECT_EQ(NC_SHORT, signedtype(NC_USHORT));
  EXPECT_EQ(NC_INT, signedtype(NC_INT));
  EXPECT_EQ(NC_INT, signedtype(NC_UINT));
  EXPECT_EQ(NC_INT64, signedtype(NC_INT64));
  EXPECT_EQ(NC_INT64, signedtype(NC_UINT64));
  EXPECT_EQ(NC_FLOAT, signedtype(NC_FLOAT));

  EXPECT_EQ(NC_UBYTE, unsignedtype(NC_BYTE));
  EXPECT_EQ(NC_UBYTE, unsignedtype(NC_UBYTE));
  EXPECT_EQ(NC_USHORT, unsignedtype(NC_SHORT));
  EXPECT_EQ(NC_USHORT, unsignedtype(NC_USHORT));
  EXPECT_EQ(NC_UINT, unsignedtype(NC_INT));
  EXPECT_EQ(NC_UINT, unsignedtype(NC_UINT));
  EXPECT_EQ(NC_UINT64, unsignedtype(NC_INT64));
  EXPECT_EQ(NC_UINT64, unsignedtype(NC_UINT64));
  EXPECT_EQ(NC_DOUBLE, unsignedtype(NC_DOUBLE));

  EXPECT_NE(0, isinttype(NC_BYTE));
  EXPECT_NE(0, isinttype(NC_INT));
  EXPECT_NE(0, isinttype(NC_UBYTE));
  EXPECT_NE(0, isinttype(NC_INT64));
  EXPECT_NE(0, isinttype(NC_UINT64));
  EXPECT_EQ(0, isinttype(NC_NAT));
  EXPECT_EQ(0, isinttype(NC_CHAR));
  EXPECT_EQ(0, isinttype(NC_FLOAT));
  EXPECT_EQ(0, isinttype(NC_STRING));

  EXPECT_NE(0, isuinttype(NC_UBYTE));
  EXPECT_NE(0, isuinttype(NC_USHORT));
  EXPECT_NE(0, isuinttype(NC_UINT));
  EXPECT_NE(0, isuinttype(NC_UINT64));
  EXPECT_EQ(0, isuinttype(NC_INT64));
  EXPECT_EQ(0, isuinttype(NC_INT));

  // Note: upstream isfloattype uses `(nctype == NC_FLOAT || nctype <=
  // NC_DOUBLE)`.
  EXPECT_NE(0, isfloattype(NC_FLOAT));
  EXPECT_NE(0, isfloattype(NC_DOUBLE));
  EXPECT_EQ(0, isfloattype(NC_INT64));
  EXPECT_EQ(0, isfloattype(NC_STRING));

  EXPECT_NE(0, isclassicprim(NC_BYTE));
  EXPECT_NE(0, isclassicprim(NC_DOUBLE));
  EXPECT_EQ(0, isclassicprim(NC_NAT));
  EXPECT_EQ(0, isclassicprim(NC_UBYTE));
  EXPECT_EQ(0, isclassicprim(NC_STRING));

  EXPECT_NE(0, isclassicprimplus(NC_BYTE));
  EXPECT_NE(0, isclassicprimplus(NC_STRING));
  EXPECT_EQ(0, isclassicprimplus(NC_UBYTE));
  EXPECT_EQ(0, isclassicprimplus(NC_VLEN));

  EXPECT_NE(0, isprim(NC_BYTE));
  EXPECT_NE(0, isprim(NC_STRING));
  EXPECT_EQ(0, isprim(NC_NAT));
  EXPECT_EQ(0, isprim(NC_VLEN));

  EXPECT_NE(0, isprimplus(NC_BYTE));
  EXPECT_NE(0, isprimplus(NC_STRING));
  EXPECT_NE(0, isprimplus(NC_ECONST));
  EXPECT_NE(0, isprimplus(NC_OPAQUE));
  EXPECT_EQ(0, isprimplus(NC_NAT));
  EXPECT_EQ(0, isprimplus(NC_COMPOUND));
}

TEST_F(UtilTest, PathPrefixFullnameAndPoolHelpersWork) {
  ScopedSymbol root = MakeScopedSymbol(NC_GRP, "root");
  ScopedSymbol g1 = MakeScopedSymbol(NC_GRP, "g1", root.get());
  ScopedSymbol g2 = MakeScopedSymbol(NC_GRP, "g2", g1.get());
  ScopedSymbol v = MakeScopedSymbol(NC_VAR, "temp", g2.get());

  List* path_list = listnew();
  const absl::Cleanup free_path_list = [&path_list] { listfree(path_list); };
  collectpath(g2.get(), path_list);
  ASSERT_EQ(3UL, listlength(path_list));
  EXPECT_EQ(g2.get(), listget(path_list, 0));
  EXPECT_EQ(g1.get(), listget(path_list, 1));
  EXPECT_EQ(root.get(), listget(path_list, 2));

  char sep[] = "/";
  EXPECT_STREQ("", prefixtostring(nullptr, sep));

  List* empty_prefix = listnew();
  const absl::Cleanup free_empty_prefix = [&empty_prefix] {
    listfree(empty_prefix);
  };
  EXPECT_STREQ("", prefixtostring(empty_prefix, sep));

  List* prefix = listnew();
  const absl::Cleanup free_prefix = [&prefix] { listfree(prefix); };
  listpush(prefix, g1.get());
  listpush(prefix, g2.get());
  EXPECT_STREQ("/g1/g2", prefixtostring(prefix, sep));

  EXPECT_STREQ("/temp", fullname(v.get()));
  v->prefix = prefix;
  EXPECT_STREQ("/g1/g2/temp", fullname(v.get()));
  v->prefix = nullptr;

  List* dup_null = prefixdup(nullptr);
  const absl::Cleanup free_dup_null = [&dup_null] { listfree(dup_null); };
  EXPECT_EQ(0UL, listlength(dup_null));

  List* dup_prefix = prefixdup(prefix);
  const absl::Cleanup free_dup_prefix = [&dup_prefix] { listfree(dup_prefix); };
  EXPECT_EQ(1, prefixeq(prefix, dup_prefix));
  EXPECT_EQ(0, prefixeq(prefix, empty_prefix));

  List* diff_prefix = listnew();
  const absl::Cleanup free_diff_prefix = [&diff_prefix] {
    listfree(diff_prefix);
  };
  listpush(diff_prefix, g1.get());
  listpush(diff_prefix, root.get());
  EXPECT_EQ(0, prefixeq(prefix, diff_prefix));

  // Exercise pool allocation, wraparound past POOLMAX (100), and concatenation.
  char* p0 = poolalloc(0);
  ASSERT_NE(nullptr, p0);
  EXPECT_EQ('\0', p0[0]);
  for (int i = 0; i < 115; ++i) {
    char* slot = poolalloc(16);
    ASSERT_NE(nullptr, slot);
  }
  EXPECT_EQ(nullptr, poolcat(nullptr, nullptr));
  EXPECT_STREQ("left", poolcat("left", nullptr));
  EXPECT_STREQ("right", poolcat(nullptr, "right"));
  EXPECT_STREQ("leftright", poolcat("left", "right"));
  EXPECT_STREQ("copyme", pooldup("copyme"));
}

TEST_F(UtilTest, MakeByteStringGetPaddingCleanupCheckErrAndKindString) {
  char hex1[] = "0a1Bff";
  size_t len1 = 0;
  std::unique_ptr<unsigned char, decltype(&std::free)> bytes1(
      makebytestring(hex1, &len1), &std::free);
  ASSERT_NE(nullptr, bytes1);
  ASSERT_EQ(3U, len1);
  EXPECT_EQ(0x0a, bytes1.get()[0]);
  EXPECT_EQ(0x1b, bytes1.get()[1]);
  EXPECT_EQ(0xff, bytes1.get()[2]);

  char hex2[] = "DEAD";
  std::unique_ptr<unsigned char, decltype(&std::free)> bytes2(
      makebytestring(hex2, nullptr), &std::free);
  ASSERT_NE(nullptr, bytes2);
  EXPECT_EQ(0xde, bytes2.get()[0]);
  EXPECT_EQ(0xad, bytes2.get()[1]);

  EXPECT_EQ(0U, getpadding(10, 0));
  EXPECT_EQ(0U, getpadding(8, 4));
  EXPECT_EQ(3U, getpadding(5, 4));
  EXPECT_EQ(7U, getpadding(1, 8));

  EXPECT_STREQ("classic", kind_string(1));
  EXPECT_STREQ("64-bit offset", kind_string(2));
  EXPECT_STREQ("netCDF-4", kind_string(3));
  EXPECT_STREQ("netCDF-4 classic model", kind_string(4));
  EXPECT_EQ(nullptr, kind_string(99));
  EXPECT_EQ(1, error_count);

  // Clean paths for check_err and check_err2 do not trigger finalize_netcdf.
  check_err(NC_NOERR, __LINE__, __FILE__, __func__);
  check_err2(NC_NOERR, 5, __LINE__, __FILE__, __func__);

  sigjmp_buf env;
  int retcode = -1;
  ncgen_shim_set_finalize_jmp(&env, &retcode);
  const absl::Cleanup disarm_jmp = [] {
    ncgen_shim_set_finalize_jmp(nullptr, nullptr);
  };

  const std::string err1_out = CaptureStderr([&] {
    if (sigsetjmp(env, 1) == 0) {
      check_err(NC_EINVAL, 101, "test_file.c", "TestFunc");
      FAIL() << "check_err(NC_EINVAL) unexpectedly returned";
    }
  });
  EXPECT_EQ(1, retcode);
  EXPECT_THAT(err1_out, ::testing::HasSubstr("ncgen: "));
  EXPECT_THAT(err1_out, ::testing::HasSubstr("test_file.c:TestFunc:101"));

  retcode = -1;
  const std::string err2_out = CaptureStderr([&] {
    if (sigsetjmp(env, 1) == 0) {
      check_err2(NC_EBADID, 12, 202, "test_file2.c", "TestFunc2");
      FAIL() << "check_err2(NC_EBADID) unexpectedly returned";
    }
  });
  EXPECT_EQ(1, retcode);
  EXPECT_THAT(err2_out, ::testing::HasSubstr("ncgen: cdl line 12; "));
  EXPECT_THAT(err2_out, ::testing::HasSubstr("test_file2.c:TestFunc2:202"));

  // Exercise cleanup() freeing symlist, *defs lists, and filldatalist.
  const absl::Cleanup reset_symbol_lists = [] {
    if (symlist != nullptr) {
      cleanup();
    }
    symlist = nullptr;
    grpdefs = nullptr;
    dimdefs = nullptr;
    attdefs = nullptr;
    gattdefs = nullptr;
    xattdefs = nullptr;
    typdefs = nullptr;
    vardefs = nullptr;
    filldatalist = nullptr;
  };
  symlist = listnew();
  grpdefs = listnew();
  dimdefs = listnew();
  attdefs = listnew();
  gattdefs = listnew();
  xattdefs = listnew();
  typdefs = listnew();
  vardefs = listnew();
  Symbol* grp = MakeRawSymbol(NC_GRP, "root");
  Symbol* var = MakeRawSymbol(NC_VAR, "v");
  listpush(symlist, grp);
  listpush(symlist, var);
  listpush(grpdefs, grp);
  listpush(vardefs, var);
  cleanup();
  symlist = nullptr;
  grpdefs = nullptr;
  dimdefs = nullptr;
  attdefs = nullptr;
  gattdefs = nullptr;
  xattdefs = nullptr;
  typdefs = nullptr;
  vardefs = nullptr;
  filldatalist = nullptr;
}

}  // namespace
}  // namespace netcdf
