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

// Clean-room unit tests for //third_party/netcdf/ncgen/semantics.c.
//
// Exercises symbol lookup/locate, topological type sorting and sizing,
// enum value/reference resolution, attribute type inference and NIL
// validation, classic-mode conversions, filename derivation, unlimited
// dimension sizing, multi-dimensional variable data packing, explode(),
// and fatal semerror() paths.

#include <csetjmp>
#include <cstddef>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"

extern "C" {
void computesize(Symbol* tsym);
Datalist* explode(NCConstant* con);
void freeSymbol(Symbol* sym);
struct yy_buffer_state;
struct yy_buffer_state* ncg_scan_string(const char* yy_str);
int ncglex_destroy(void);
}  // extern "C"

namespace netcdf {
namespace {

Symbol* MakeHeapSymbol(const char* name, nc_class objectclass,
                       nc_class subclass = NC_NAT) {
  auto* sym = static_cast<Symbol*>(chkcalloc(sizeof(Symbol)));
  sym->name = chkstrdup(name);
  sym->objectclass = objectclass;
  sym->subclass = subclass;
  return sym;
}

// POD-only wrapper around sigsetjmp/siglongjmp so no non-trivial C++ objects
// live in the sigsetjmp frame when semerror() invokes finalize_netcdf(1).
int InvokeWithSemerrorTrap(void (*fn)(void*), void* ctx) {
  sigjmp_buf env;
  int retcode = -1;
  ncgen_shim_set_finalize_jmp(&env, &retcode);
  if (sigsetjmp(env, 1) == 0) {
    fn(ctx);
    ncgen_shim_set_finalize_jmp(nullptr, nullptr);
    return 0;
  }
  ncgen_shim_set_finalize_jmp(nullptr, nullptr);
  return retcode == 0 ? 1 : retcode;
}

void CallProcessSemanticsTrapped(void* /*unused*/) { processsemantics(); }

struct ParseCdlOptions {
  const char* cdl = nullptr;
  int kflag = NC_FORMAT_NETCDF4;
  int classic = 0;
  Language lang = L_BINARY;
  char* out_name = nullptr;
  char* in_name = nullptr;
};

template <typename HookFn, typename VerifyFn>
::testing::AssertionResult RunCdlSemanticsInChild(const ParseCdlOptions& opts,
                                                  HookFn&& pre_hook,
                                                  VerifyFn&& verify_fn) {
  return RunInForkedChild([&]() -> bool {
    ncgen_shim_reset_globals();
    k_flag = opts.kflag;
    usingclassic = opts.classic;
    l_flag = opts.lang;
    netcdf_name = opts.out_name;
    cdlname = opts.in_name;

    parse_init();
    if (ncg_scan_string(opts.cdl) == nullptr) {
      return false;
    }
    if (ncgparse() != 0 || error_count != 0) {
      ncglex_destroy();
      cleanup();
      nullfree(datasetname);
      datasetname = nullptr;
      return false;
    }
    if (!pre_hook()) {
      ncglex_destroy();
      cleanup();
      nullfree(datasetname);
      datasetname = nullptr;
      return false;
    }
    processsemantics();
    const bool ok = verify_fn();
    ncglex_destroy();
    cleanup();
    nullfree(datasetname);
    datasetname = nullptr;
    return ok && error_count == 0;
  });
}

template <typename HookFn>
::testing::AssertionResult ExpectSemerrorInChild(const ParseCdlOptions& opts,
                                                 HookFn&& pre_hook) {
  return RunInForkedChild([&]() -> bool {
    ncgen_shim_reset_globals();
    char default_cdl[] = "-";
    k_flag = opts.kflag;
    usingclassic = opts.classic;
    l_flag = opts.lang;
    netcdf_name = opts.out_name;
    cdlname = opts.in_name != nullptr ? opts.in_name : default_cdl;

    parse_init();
    if (ncg_scan_string(opts.cdl) == nullptr) {
      return false;
    }
    if (ncgparse() != 0 || error_count != 0) {
      ncglex_destroy();
      return false;
    }
    if (!pre_hook()) {
      ncglex_destroy();
      return false;
    }
    const int rc = InvokeWithSemerrorTrap(CallProcessSemanticsTrapped, nullptr);
    ncglex_destroy();
    return rc != 0 && error_count > 0;
  });
}

TEST(SemanticsTest, NctypesizeReturnsExpectedSizes) {
  EXPECT_EQ(sizeof(char), nctypesize(NC_BYTE));
  EXPECT_EQ(sizeof(char), nctypesize(NC_CHAR));
  EXPECT_EQ(sizeof(short), nctypesize(NC_SHORT));
  EXPECT_EQ(sizeof(int), nctypesize(NC_INT));
  EXPECT_EQ(sizeof(float), nctypesize(NC_FLOAT));
  EXPECT_EQ(sizeof(double), nctypesize(NC_DOUBLE));
  EXPECT_EQ(sizeof(unsigned char), nctypesize(NC_UBYTE));
  EXPECT_EQ(sizeof(unsigned short), nctypesize(NC_USHORT));
  EXPECT_EQ(sizeof(unsigned int), nctypesize(NC_UINT));
  EXPECT_EQ(sizeof(long long), nctypesize(NC_INT64));
  EXPECT_EQ(sizeof(unsigned long long), nctypesize(NC_UINT64));
  EXPECT_EQ(sizeof(char*), nctypesize(NC_STRING));
}

TEST(SemanticsTest, LookupHelpersHandleNullPrefixRefsAndMismatches) {
  std::vector<Symbol*> owned_syms;
  std::vector<List*> owned_lists;
  Symbol* const saved_root = rootgroup;
  absl::Cleanup cleanup_guard = [&]() {
    rootgroup = saved_root;
    for (Symbol* sym : owned_syms) {
      freeSymbol(sym);
    }
    for (List* list : owned_lists) {
      listfree(list);
    }
  };

  Symbol* root = MakeHeapSymbol("root", NC_GRP);
  root->subnodes = listnew();
  owned_syms.push_back(root);
  rootgroup = root;

  Symbol* grp = MakeHeapSymbol("sub", NC_GRP);
  grp->container = root;
  grp->subnodes = listnew();
  owned_syms.push_back(grp);

  // Reference symbol with same name and class must be skipped.
  Symbol* var_ref = MakeHeapSymbol("target", NC_VAR);
  var_ref->ref.is_ref = 1;
  owned_syms.push_back(var_ref);

  // Real symbol with different objectclass must be skipped.
  Symbol* dim_same_name = MakeHeapSymbol("target", NC_DIM);
  owned_syms.push_back(dim_same_name);

  // Real symbol with different name in target class must be skipped.
  Symbol* var_other = MakeHeapSymbol("other", NC_VAR);
  owned_syms.push_back(var_other);

  // Actual matching variable.
  Symbol* var_target = MakeHeapSymbol("target", NC_VAR);
  owned_syms.push_back(var_target);

  listpush(root->subnodes, grp);
  listpush(grp->subnodes, var_ref);
  listpush(grp->subnodes, dim_same_name);
  listpush(grp->subnodes, var_other);
  listpush(grp->subnodes, var_target);

  // lookupgroup() handles NULL, empty, and non-empty prefix lists.
  EXPECT_EQ(root, lookupgroup(nullptr));
  List* empty_prefix = listnew();
  owned_lists.push_back(empty_prefix);
  EXPECT_EQ(root, lookupgroup(empty_prefix));

  List* grp_prefix = listnew();
  owned_lists.push_back(grp_prefix);
  listpush(grp_prefix, grp);
  EXPECT_EQ(grp, lookupgroup(grp_prefix));

  // lookupingroup() validates NULL name, NULL grp fallback, and match filters.
  EXPECT_EQ(nullptr, lookupingroup(NC_VAR, nullptr, grp));
  char grp_name[] = "sub";
  EXPECT_EQ(grp, lookupingroup(NC_GRP, grp_name, nullptr));

  char target_name[] = "target";
  char missing_name[] = "missing";
  EXPECT_EQ(var_target, lookupingroup(NC_VAR, target_name, grp));
  EXPECT_EQ(nullptr, lookupingroup(NC_VAR, missing_name, grp));

  // lookup() checks NULL pattern, NULL group from listtop, and prefixed match.
  EXPECT_EQ(nullptr, lookup(NC_VAR, nullptr));

  Symbol* pattern = MakeHeapSymbol("target", NC_VAR);
  pattern->prefix = listnew();
  owned_syms.push_back(pattern);

  List* null_top_prefix = listnew();
  owned_lists.push_back(null_top_prefix);
  listpush(null_top_prefix, nullptr);
  List* saved_pattern_prefix = pattern->prefix;
  pattern->prefix = null_top_prefix;
  EXPECT_EQ(nullptr, lookup(NC_VAR, pattern));

  pattern->prefix = saved_pattern_prefix;
  listpush(pattern->prefix, grp);
  EXPECT_EQ(var_target, lookup(NC_VAR, pattern));
}

TEST(SemanticsTest,
     LocateResolvesPrefixedPrimitivesHierarchyUniqueTreeAndDebug) {
  std::vector<Symbol*> owned_syms;
  Symbol* const saved_root = rootgroup;
  const int saved_debug = debug;
  Symbol* saved_prims[PRIMNO];
  std::memcpy(saved_prims, primsymbols, sizeof(saved_prims));

  absl::Cleanup cleanup_guard = [&]() {
    rootgroup = saved_root;
    debug = saved_debug;
    std::memcpy(primsymbols, saved_prims, sizeof(saved_prims));
    for (Symbol* sym : owned_syms) {
      freeSymbol(sym);
    }
  };

  for (int i = 0; i < PRIMNO; ++i) {
    primsymbols[i] = nullptr;
  }
  Symbol* prim_int = MakeHeapSymbol("int", NC_TYPE, NC_PRIM);
  prim_int->typ.typecode = NC_INT;
  prim_int->typ.size = sizeof(int);
  primsymbols[NC_INT] = prim_int;
  owned_syms.push_back(prim_int);

  Symbol* root = MakeHeapSymbol("root", NC_GRP);
  root->subnodes = listnew();
  owned_syms.push_back(root);
  rootgroup = root;

  Symbol* g_a = MakeHeapSymbol("ga", NC_GRP);
  g_a->container = root;
  g_a->subnodes = listnew();
  owned_syms.push_back(g_a);

  Symbol* g_a_child = MakeHeapSymbol("ga_child", NC_GRP);
  g_a_child->container = g_a;
  g_a_child->subnodes = listnew();
  owned_syms.push_back(g_a_child);

  Symbol* g_b = MakeHeapSymbol("gb", NC_GRP);
  g_b->container = root;
  g_b->subnodes = listnew();
  owned_syms.push_back(g_b);

  // Include a reference group node inside root to exercise uniquetreelocate().
  Symbol* grp_ref_node = MakeHeapSymbol("g_ref", NC_GRP);
  grp_ref_node->ref.is_ref = 1;
  owned_syms.push_back(grp_ref_node);

  Symbol* dim_root = MakeHeapSymbol("d_root", NC_DIM);
  owned_syms.push_back(dim_root);
  Symbol* dim_a = MakeHeapSymbol("d_a", NC_DIM);
  owned_syms.push_back(dim_a);

  Symbol* type_root = MakeHeapSymbol("t_root", NC_TYPE, NC_OPAQUE);
  owned_syms.push_back(type_root);
  Symbol* type_unique = MakeHeapSymbol("t_unique", NC_TYPE, NC_OPAQUE);
  owned_syms.push_back(type_unique);
  Symbol* type_dup_a = MakeHeapSymbol("t_dup", NC_TYPE, NC_OPAQUE);
  owned_syms.push_back(type_dup_a);
  Symbol* type_dup_b = MakeHeapSymbol("t_dup", NC_TYPE, NC_OPAQUE);
  owned_syms.push_back(type_dup_b);

  Symbol* var_a = MakeHeapSymbol("v_a", NC_VAR);
  owned_syms.push_back(var_a);

  listpush(root->subnodes, g_a);
  listpush(root->subnodes, g_b);
  listpush(root->subnodes, grp_ref_node);
  listpush(root->subnodes, dim_root);
  listpush(root->subnodes, type_root);
  listpush(g_a->subnodes, g_a_child);
  listpush(g_a->subnodes, dim_a);
  listpush(g_a->subnodes, var_a);
  listpush(g_a->subnodes, type_dup_a);
  listpush(g_b->subnodes, type_unique);
  listpush(g_b->subnodes, type_dup_b);

  // Enable debug > 1 to cover fdebug/dumpgroup logging in locate/lookupingroup.
  debug = 2;

  auto make_ref = [&](const char* name, nc_class cls, nc_class sub,
                      Symbol* prefix_grp, int prefixed) -> Symbol* {
    Symbol* ref = MakeHeapSymbol(name, cls, sub);
    ref->ref.is_ref = 1;
    ref->is_prefixed = prefixed;
    ref->prefix = listnew();
    if (prefix_grp != nullptr) {
      listpush(ref->prefix, prefix_grp);
    }
    owned_syms.push_back(ref);
    return ref;
  };

  // NC_DIM: unprefixed from child climbs to root; prefixed stays exact.
  EXPECT_EQ(dim_root, locate(make_ref("d_root", NC_DIM, NC_NAT, g_a_child, 0)));
  EXPECT_EQ(nullptr, locate(make_ref("d_none", NC_DIM, NC_NAT, g_a_child, 0)));
  EXPECT_EQ(dim_a, locate(make_ref("d_a", NC_DIM, NC_NAT, g_a, 1)));
  EXPECT_EQ(nullptr, locate(make_ref("d_root", NC_DIM, NC_NAT, g_a, 1)));

  // NC_TYPE: primitive shortcut (skips NC_NAT), parent climb, uniquetreelocate,
  // duplicate non-unique rejection, and prefixed lookup.
  EXPECT_EQ(prim_int, locate(make_ref("int", NC_TYPE, NC_PRIM, g_a_child, 0)));
  EXPECT_EQ(type_root,
            locate(make_ref("t_root", NC_TYPE, NC_OPAQUE, g_a_child, 0)));
  EXPECT_EQ(type_unique,
            locate(make_ref("t_unique", NC_TYPE, NC_OPAQUE, g_a_child, 0)));
  EXPECT_EQ(type_dup_a,
            locate(make_ref("t_dup", NC_TYPE, NC_OPAQUE, g_a_child, 0)));
  EXPECT_EQ(nullptr, locate(make_ref("t_dup", NC_TYPE, NC_OPAQUE, root, 0)));
  EXPECT_EQ(type_dup_b, locate(make_ref("t_dup", NC_TYPE, NC_OPAQUE, g_b, 1)));

  // NC_VAR: unprefixed and prefixed.
  EXPECT_EQ(var_a, locate(make_ref("v_a", NC_VAR, NC_NAT, g_a, 0)));
  EXPECT_EQ(var_a, locate(make_ref("v_a", NC_VAR, NC_NAT, g_a, 1)));
  EXPECT_EQ(nullptr, locate(make_ref("v_a", NC_VAR, NC_NAT, g_b, 0)));

  // NC_GRP: unprefixed and prefixed.
  EXPECT_EQ(g_a, locate(make_ref("ga", NC_GRP, NC_NAT, nullptr, 0)));
  EXPECT_EQ(g_a_child, locate(make_ref("ga_child", NC_GRP, NC_NAT, g_a, 1)));
}

TEST(SemanticsTest, ComputesizeHandlesAllClassesAlignmentPaddingAndTouched) {
  std::vector<Symbol*> owned_syms;
  absl::Cleanup cleanup_guard = [&]() {
    for (Symbol* sym : owned_syms) {
      freeSymbol(sym);
    }
  };

  Symbol* byte_prim = MakeHeapSymbol("byte", NC_TYPE, NC_PRIM);
  byte_prim->typ.typecode = NC_BYTE;
  owned_syms.push_back(byte_prim);
  computesize(byte_prim);
  EXPECT_EQ(size_t{1}, byte_prim->typ.size);
  EXPECT_EQ(size_t{1}, byte_prim->typ.nelems);
  // Second call returns early on already-touched symbol.
  computesize(byte_prim);
  EXPECT_EQ(1, byte_prim->touched);

  Symbol* short_prim = MakeHeapSymbol("short", NC_TYPE, NC_PRIM);
  short_prim->typ.typecode = NC_SHORT;
  owned_syms.push_back(short_prim);

  Symbol* int_prim = MakeHeapSymbol("int", NC_TYPE, NC_PRIM);
  int_prim->typ.typecode = NC_INT;
  owned_syms.push_back(int_prim);
  computesize(int_prim);

  // NC_OPAQUE: size/alignment preset, nelems set to 1.
  Symbol* opq = MakeHeapSymbol("opq7", NC_TYPE, NC_OPAQUE);
  opq->typ.typecode = NC_OPAQUE;
  opq->typ.size = 7;
  opq->typ.alignment = 1;
  owned_syms.push_back(opq);
  computesize(opq);
  EXPECT_EQ(size_t{7}, opq->typ.size);
  EXPECT_EQ(size_t{1}, opq->typ.nelems);

  // NC_ENUM recurses to basetype (short_prim not yet touched).
  Symbol* enm = MakeHeapSymbol("es", NC_TYPE, NC_ENUM);
  enm->typ.typecode = NC_ENUM;
  enm->typ.basetype = short_prim;
  owned_syms.push_back(enm);
  computesize(enm);
  EXPECT_EQ(sizeof(short), enm->typ.size);
  EXPECT_EQ(short_prim->typ.alignment, enm->typ.alignment);
  EXPECT_EQ(size_t{1}, enm->typ.nelems);

  // NC_VLEN recurses to basetype (new primitive untouched first).
  Symbol* dbl_prim = MakeHeapSymbol("double", NC_TYPE, NC_PRIM);
  dbl_prim->typ.typecode = NC_DOUBLE;
  owned_syms.push_back(dbl_prim);

  Symbol* vlen = MakeHeapSymbol("vd", NC_TYPE, NC_VLEN);
  vlen->typ.typecode = NC_VLEN;
  vlen->typ.basetype = dbl_prim;
  owned_syms.push_back(vlen);
  computesize(vlen);
  EXPECT_EQ(1, dbl_prim->touched);
  EXPECT_EQ(ncsize(NC_VLEN), vlen->typ.size);
  EXPECT_EQ(size_t{1}, vlen->typ.nelems);

  // NC_COMPOUND with exact size multiple (rem == 0).
  Symbol* f_a = MakeHeapSymbol("a", NC_TYPE, NC_FIELD);
  f_a->typ.basetype = int_prim;
  owned_syms.push_back(f_a);
  Symbol* f_b = MakeHeapSymbol("b", NC_TYPE, NC_FIELD);
  f_b->typ.basetype = int_prim;
  owned_syms.push_back(f_b);

  Symbol* cmpd_exact = MakeHeapSymbol("cmpd_exact", NC_TYPE, NC_COMPOUND);
  cmpd_exact->subnodes = listnew();
  listpush(cmpd_exact->subnodes, f_a);
  listpush(cmpd_exact->subnodes, f_b);
  owned_syms.push_back(cmpd_exact);
  computesize(cmpd_exact);
  EXPECT_EQ(0UL, f_a->typ.offset);
  EXPECT_EQ(static_cast<unsigned long>(sizeof(int)), f_b->typ.offset);
  EXPECT_EQ(2 * sizeof(int), cmpd_exact->typ.size);
  EXPECT_EQ(int_prim->typ.alignment, cmpd_exact->typ.cmpdalign);

  // NC_COMPOUND with interior padding, increasing alignment, decreasing
  // alignment, array field (dimset.ndims > 0), and trailing padding (rem != 0).
  Symbol* d3 = MakeHeapSymbol("const3", NC_DIM);
  d3->dim.isconstant = 1;
  d3->dim.declsize = 3;
  owned_syms.push_back(d3);

  Symbol* byte2_prim = MakeHeapSymbol("byte2", NC_TYPE, NC_PRIM);
  byte2_prim->typ.typecode = NC_BYTE;
  owned_syms.push_back(byte2_prim);

  Symbol* f_head = MakeHeapSymbol("head", NC_TYPE, NC_FIELD);
  f_head->typ.basetype = byte_prim;
  owned_syms.push_back(f_head);

  Symbol* f_mid = MakeHeapSymbol("mid", NC_TYPE, NC_FIELD);
  f_mid->typ.basetype = int_prim;
  owned_syms.push_back(f_mid);

  Symbol* f_arr = MakeHeapSymbol("arr", NC_TYPE, NC_FIELD);
  f_arr->typ.basetype = byte2_prim;
  f_arr->typ.dimset.ndims = 1;
  f_arr->typ.dimset.dimsyms[0] = d3;
  owned_syms.push_back(f_arr);

  Symbol* cmpd_padded = MakeHeapSymbol("cmpd_padded", NC_TYPE, NC_COMPOUND);
  cmpd_padded->subnodes = listnew();
  listpush(cmpd_padded->subnodes, f_head);
  listpush(cmpd_padded->subnodes, f_mid);
  listpush(cmpd_padded->subnodes, f_arr);
  owned_syms.push_back(cmpd_padded);
  computesize(cmpd_padded);

  const size_t expected_mid_off = 1 + getpadding(1, int_prim->typ.alignment);
  const size_t expected_arr_off = expected_mid_off + sizeof(int);
  const size_t raw_end = expected_arr_off + 3;
  const size_t expected_size =
      raw_end + getpadding(raw_end, cmpd_padded->typ.cmpdalign);
  EXPECT_EQ(0UL, f_head->typ.offset);
  EXPECT_EQ(static_cast<unsigned long>(expected_mid_off), f_mid->typ.offset);
  EXPECT_EQ(static_cast<unsigned long>(expected_arr_off), f_arr->typ.offset);
  EXPECT_EQ(size_t{3}, f_arr->typ.size);
  EXPECT_EQ(int_prim->typ.alignment, cmpd_padded->typ.cmpdalign);
  EXPECT_EQ(expected_size, cmpd_padded->typ.size);
}

TEST(SemanticsTest, ExplodeConvertsStringConstantToCharDatalist) {
  NCConstant* str_con = nullconst();
  str_con->nctype = NC_STRING;
  str_con->value.stringv.stringv = chkstrdup("abZ");
  str_con->value.stringv.len = 3;

  Datalist* chars = explode(str_con);
  ASSERT_NE(nullptr, chars);
  ASSERT_EQ(size_t{3}, datalistlen(chars));
  EXPECT_EQ(NC_CHAR, datalistith(chars, 0)->nctype);
  EXPECT_EQ('a', datalistith(chars, 0)->value.charv);
  EXPECT_EQ('b', datalistith(chars, 1)->value.charv);
  EXPECT_EQ('Z', datalistith(chars, 2)->value.charv);
  reclaimdatalist(chars);
  reclaimconstant(str_con);

  NCConstant* empty_con = nullconst();
  empty_con->nctype = NC_STRING;
  empty_con->value.stringv.stringv = chkstrdup("");
  empty_con->value.stringv.len = 0;
  Datalist* empty_chars = explode(empty_con);
  ASSERT_NE(nullptr, empty_chars);
  EXPECT_EQ(size_t{0}, datalistlen(empty_chars));
  reclaimdatalist(empty_chars);
  reclaimconstant(empty_con);
}

TEST(SemanticsTest, CreateFilenameHandlesAllPrecedenceAndPathForms) {
  constexpr const char* kMinimalCdl = "netcdf ds_name { dimensions: d = 1; }\n";

  // 1. Explicit netcdf_name takes precedence over cdlname and datasetname.
  {
    char out_name[] = "custom_out.nc";
    char in_name[] = "dir/ignored.cdl";
    ParseCdlOptions opts{kMinimalCdl, NC_FORMAT_NETCDF4, 0,
                         L_BINARY,    out_name,          in_name};
    ASSERT_TRUE(RunCdlSemanticsInChild(
        opts, []() { return true; },
        []() {
          return std::strcmp(rootgroup->file.filename, "custom_out.nc") == 0;
        }));
  }

  // 2. cdlname == "-" falls back to datasetname + binary_ext.
  {
    char dash[] = "-";
    ParseCdlOptions opts{kMinimalCdl, NC_FORMAT_NETCDF4, 0,
                         L_BINARY,    nullptr,           dash};
    ASSERT_TRUE(RunCdlSemanticsInChild(
        opts, []() { return true; },
        []() {
          return std::strcmp(rootgroup->file.filename, "ds_name.nc") == 0;
        }));
  }

  // 3. cdlname == nullptr falls back to datasetname + binary_ext.
  {
    ParseCdlOptions opts{kMinimalCdl, NC_FORMAT_NETCDF4, 0,
                         L_BINARY,    nullptr,           nullptr};
    ASSERT_TRUE(RunCdlSemanticsInChild(
        opts, []() { return true; },
        []() {
          return std::strcmp(rootgroup->file.filename, "ds_name.nc") == 0;
        }));
  }

  // 4. cdlname with directory prefix and suffix strips both.
  {
    char in_path[] = "alpha/beta/sample.cdl";
    ParseCdlOptions opts{kMinimalCdl, NC_FORMAT_NETCDF4, 0,
                         L_BINARY,    nullptr,           in_path};
    ASSERT_TRUE(RunCdlSemanticsInChild(
        opts, []() { return true; },
        []() {
          return std::strcmp(rootgroup->file.filename, "sample.nc") == 0;
        }));
  }

  // 5. cdlname without '/' or '.' preserves basename and appends binary_ext.
  {
    char bare_name[] = "plain_input";
    ParseCdlOptions opts{kMinimalCdl, NC_FORMAT_NETCDF4, 0,
                         L_BINARY,    nullptr,           bare_name};
    ASSERT_TRUE(RunCdlSemanticsInChild(
        opts, []() { return true; },
        []() {
          return std::strcmp(rootgroup->file.filename, "plain_input.nc") == 0;
        }));
  }

  // 6. cdlname with '/' only (no '.') and with '.' only (no '/').
  {
    char dir_only[] = "dir_only/basename_no_dot";
    ParseCdlOptions opts{kMinimalCdl, NC_FORMAT_NETCDF4, 0,
                         L_BINARY,    nullptr,           dir_only};
    ASSERT_TRUE(RunCdlSemanticsInChild(
        opts, []() { return true; },
        []() {
          return std::strcmp(rootgroup->file.filename, "basename_no_dot.nc") ==
                 0;
        }));
  }
  {
    char dot_only[] = "file_only.cdl";
    ParseCdlOptions opts{kMinimalCdl, NC_FORMAT_NETCDF4, 0,
                         L_BINARY,    nullptr,           dot_only};
    ASSERT_TRUE(RunCdlSemanticsInChild(
        opts, []() { return true; },
        []() {
          return std::strcmp(rootgroup->file.filename, "file_only.nc") == 0;
        }));
  }
}

TEST(SemanticsTest, Netcdf4TypesHierarchyCompoundsEnumsVlensAndOpaques) {
  constexpr const char* kCdl = R"(
netcdf nc4_types {
types:
  opaque(11) opq_t;
  byte enum e_byte { EB_A = -3, EB_B = 4 };
  ubyte enum e_ubyte { EUB_A = 1, EUB_B = 250 };
  short enum e_short { ES_A = -10, ES_B = 20 };
  ushort enum e_ushort { EUS_A = 100 };
  int enum e_int { EI_A = -5, EI_B = 5 };
  uint enum e_uint { EUI_A = 7 };
  int64 enum e_int64 { EI64_A = 8 };
  uint64 enum e_uint64 { EUI64_A = 9 };
  int(*) v1_t;
  v1_t(*) v2_t;
  v2_t(*) v3_t;
  compound c_prim_t { byte b; int i; short s; };
  compound c_ok_t { int i; v1_t x; };
  compound c_nested_t { v1_t a; v2_t b; int arr(2, 2); };
dimensions:
  d2 = 2;
variables:
  opq_t vo;
  c_prim_t vcp;
  c_nested_t vcn;
  v2_t vv2(d2);
  v3_t vv3;
data:
  vcp = {1b, 2, 3s};
group: g1 {
dimensions:
  gd = 2;
variables:
  c_nested_t gv(gd);
}
}
)";

  ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
  ASSERT_TRUE(RunCdlSemanticsInChild(
      opts,
      []() {
        // Reorder [v2_t, v3_t, c_ok_t, c_nested_t] -> [c_ok_t, v3_t,
        // c_nested_t, v2_t] so Pass-2 processtypes() exercises:
        // - c_ok_t completing all-touched compound loop without break,
        // - v3_t hitting untouched vlen check then succeeding on next
        // iteration,
        // - c_nested_t breaking on untouched field v2_t,
        // - v2_t hitting touched vlen check.
        // Also push primsymbols[NC_SHORT] to cover NC_PRIM filtering.
        Symbol* v2 = nullptr;
        Symbol* v3 = nullptr;
        Symbol* co = nullptr;
        Symbol* cn = nullptr;
        unsigned long idx[4] = {0, 0, 0, 0};
        for (unsigned long i = 0; i < listlength(typdefs); ++i) {
          auto* s = static_cast<Symbol*>(listget(typdefs, i));
          if (std::strcmp(s->name, "v2_t") == 0) {
            v2 = s;
            idx[0] = i;
          } else if (std::strcmp(s->name, "v3_t") == 0) {
            v3 = s;
            idx[1] = i;
          } else if (std::strcmp(s->name, "c_ok_t") == 0) {
            co = s;
            idx[2] = i;
          } else if (std::strcmp(s->name, "c_nested_t") == 0) {
            cn = s;
            idx[3] = i;
          }
        }
        if (!v2 || !v3 || !co || !cn) return false;
        if (!(idx[0] < idx[1] && idx[1] < idx[2] && idx[2] < idx[3])) {
          return false;
        }
        listset(typdefs, idx[0], co);
        listset(typdefs, idx[1], v3);
        listset(typdefs, idx[2], cn);
        listset(typdefs, idx[3], v2);
        if (primsymbols[NC_SHORT]->fqn == nullptr) {
          primsymbols[NC_SHORT]->fqn = chkstrdup("/short");
        }
        listpush(typdefs, primsymbols[NC_SHORT]);
        return true;
      },
      []() {
        Symbol* v1 = nullptr;
        Symbol* v2 = nullptr;
        Symbol* v3 = nullptr;
        Symbol* co = nullptr;
        Symbol* cn = nullptr;
        Symbol* cp = nullptr;
        Symbol* eb = nullptr;
        for (unsigned long i = 0; i < listlength(typdefs); ++i) {
          auto* s = static_cast<Symbol*>(listget(typdefs, i));
          if (std::strcmp(s->name, "v1_t") == 0) v1 = s;
          if (std::strcmp(s->name, "v2_t") == 0) v2 = s;
          if (std::strcmp(s->name, "v3_t") == 0) v3 = s;
          if (std::strcmp(s->name, "c_ok_t") == 0) co = s;
          if (std::strcmp(s->name, "c_nested_t") == 0) cn = s;
          if (std::strcmp(s->name, "c_prim_t") == 0) cp = s;
          if (std::strcmp(s->name, "e_byte") == 0) eb = s;
          // Predefined NC_PRIM symbol should have been excluded from sorted
          // typdefs before checkconsistency().
          if (s->subclass == NC_PRIM) return false;
        }
        if (!v1 || !v2 || !v3 || !co || !cn || !cp || !eb) return false;
        if (v1->typ.hasvlen != 1 || v2->typ.hasvlen != 1 ||
            v3->typ.hasvlen != 1 || co->typ.hasvlen != 1 ||
            cn->typ.hasvlen != 1) {
          return false;
        }
        if (cp->typ.hasvlen != 0) return false;
        if (eb->typ.size != 1) return false;
        return eb->fqn != nullptr && cn->fqn != nullptr && v3->fqn != nullptr;
      }));
}

TEST(SemanticsTest, EnumConstantResolutionAcrossGroupsAttributesAndFillValue) {
  constexpr const char* kCdl =
      "netcdf enums_test {\n"
      "types:\n"
      "  int enum e_root { R_A = 10, R_B = 20 };\n"
      "  short enum e_pref { P_A = 1, P_B = 2 };\n"
      "  compound c_enum { int tag; e_root code; };\n"
      "dimensions:\n"
      "  d2 = 2;\n"
      "variables:\n"
      "  e_root v_root(d2);\n"
      "  e_pref v_pref;\n"
      "  e_pref v_pref_fallback;\n"
      "  c_enum v_cmpd;\n"
      "  int v_subref;\n"
      "  e_root :g_econst = R_A;\n"
      "  e_root v_root:v_econst = R_B;\n"
      "  v_root:" NC_FillValue
      " = R_A;\n"
      "data:\n"
      "  v_root = R_A, R_B;\n"
      "  v_pref = P_B;\n"
      "  v_pref_fallback = P_B;\n"
      "  v_cmpd = {1, R_B};\n"
      "  v_subref = D_VAL;\n"
      "group: g1 {\n"
      "types:\n"
      "  int enum e_ignored_non_enum { UNUSED = 0 };\n"
      "  int enum e_dup_ec { P_B = 99 };\n"
      "  opaque(4) opq_in_g1;\n"
      "group: g2 {\n"
      "types:\n"
      "  int enum e_deep { D_VAL = 77 };\n"
      "variables:\n"
      "  int v_in_g2;\n"
      "  e_deep v_in_g2:g2_vatt = D_VAL;\n"
      "}\n"
      "}\n"
      "}\n";

  ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
  ASSERT_TRUE(RunCdlSemanticsInChild(
      opts,
      []() {
        // Rewrite v_pref's enum constant reference to "e_pref.P_B" (exact
        // match) and v_pref_fallback's to "nonexistent.P_B" (tests fallback to
        // candidate when explicit EnumType prefix is absent, plus
        // duplicate-candidate skip). Also repoint g2_vatt->container to v_in_g2
        // (NC_VAR) to exercise the container->objectclass == NC_VAR step in
        // orderedtypes().
        bool rewrote_pref = false;
        bool rewrote_fallback = false;
        Symbol* v_in_g2 = nullptr;
        for (unsigned long i = 0; i < listlength(vardefs); ++i) {
          auto* var = static_cast<Symbol*>(listget(vardefs, i));
          if (std::strcmp(var->name, "v_pref") == 0 && var->data != nullptr &&
              datalistlen(var->data) == 1) {
            NCConstant* con = datalistith(var->data, 0);
            if (con == nullptr || con->nctype != NC_ECONST ||
                con->value.enumv == nullptr) {
              return false;
            }
            efree(con->value.enumv->name);
            con->value.enumv->name = chkstrdup("e_pref.P_B");
            rewrote_pref = true;
          } else if (std::strcmp(var->name, "v_pref_fallback") == 0 &&
                     var->data != nullptr && datalistlen(var->data) == 1) {
            NCConstant* con = datalistith(var->data, 0);
            if (con == nullptr || con->nctype != NC_ECONST ||
                con->value.enumv == nullptr) {
              return false;
            }
            efree(con->value.enumv->name);
            con->value.enumv->name = chkstrdup("nonexistent.P_B");
            rewrote_fallback = true;
          } else if (std::strcmp(var->name, "v_in_g2") == 0) {
            v_in_g2 = var;
          }
        }
        bool repointed_att = false;
        for (unsigned long i = 0; i < listlength(attdefs); ++i) {
          auto* att = static_cast<Symbol*>(listget(attdefs, i));
          if (std::strcmp(att->name, "g2_vatt") == 0 && v_in_g2 != nullptr) {
            att->container = v_in_g2;
            repointed_att = true;
          }
        }
        return rewrote_pref && rewrote_fallback && repointed_att;
      },
      []() {
        auto check_scalar_ec = [](Symbol* var, const char* expected_name,
                                  long long expected_val) -> bool {
          if (var == nullptr || var->data == nullptr ||
              datalistlen(var->data) != 1) {
            return false;
          }
          NCConstant* c = datalistith(var->data, 0);
          if (c == nullptr || c->nctype != NC_ECONST ||
              c->value.enumv == nullptr ||
              c->value.enumv->subclass != NC_ECONST ||
              std::strcmp(c->value.enumv->name, expected_name) != 0) {
            return false;
          }
          NCConstant* ev = c->value.enumv->typ.econst;
          if (ev == nullptr) return false;
          if (ev->nctype == NC_SHORT) return ev->value.int16v == expected_val;
          if (ev->nctype == NC_INT) return ev->value.int32v == expected_val;
          return false;
        };

        Symbol* v_pref = nullptr;
        Symbol* v_pref_fallback = nullptr;
        Symbol* v_subref = nullptr;
        Symbol* v_root = nullptr;
        Symbol* v_cmpd = nullptr;
        for (unsigned long i = 0; i < listlength(vardefs); ++i) {
          auto* var = static_cast<Symbol*>(listget(vardefs, i));
          if (std::strcmp(var->name, "v_pref") == 0) v_pref = var;
          if (std::strcmp(var->name, "v_pref_fallback") == 0)
            v_pref_fallback = var;
          if (std::strcmp(var->name, "v_subref") == 0) v_subref = var;
          if (std::strcmp(var->name, "v_root") == 0) v_root = var;
          if (std::strcmp(var->name, "v_cmpd") == 0) v_cmpd = var;
        }
        if (!check_scalar_ec(v_pref, "P_B", 2) ||
            !check_scalar_ec(v_pref_fallback, "P_B", 2) ||
            !check_scalar_ec(v_subref, "D_VAL", 77)) {
          return false;
        }

        // Check 1D array variable and NC_FillValue special datalist.
        if (v_root == nullptr || v_root->data == nullptr ||
            datalistlen(v_root->data) != 2 ||
            v_root->var.special._Fillvalue == nullptr ||
            datalistlen(v_root->var.special._Fillvalue) != 1) {
          return false;
        }
        NCConstant* rb = datalistith(v_root->data, 1);
        NCConstant* rf = datalistith(v_root->var.special._Fillvalue, 0);
        if (rb == nullptr || rb->nctype != NC_ECONST ||
            rb->value.enumv->typ.econst->value.int32v != 20) {
          return false;
        }
        if (rf == nullptr || rf->nctype != NC_ECONST ||
            rf->value.enumv->typ.econst->value.int32v != 10) {
          return false;
        }

        // Check nested compound enum constant resolution.
        if (v_cmpd == nullptr || v_cmpd->data == nullptr ||
            datalistlen(v_cmpd->data) != 1) {
          return false;
        }
        Datalist* fields = compoundfor(datalistith(v_cmpd->data, 0));
        if (fields == nullptr || datalistlen(fields) != 2) return false;
        NCConstant* cmpd_ec = datalistith(fields, 1);
        if (cmpd_ec == nullptr || cmpd_ec->nctype != NC_ECONST ||
            cmpd_ec->value.enumv->typ.econst->value.int32v != 20) {
          return false;
        }

        // Check global and repointed group-variable enum attributes.
        bool saw_gatt = false;
        bool saw_g2_vatt = false;
        for (unsigned long i = 0; i < listlength(gattdefs); ++i) {
          auto* att = static_cast<Symbol*>(listget(gattdefs, i));
          if (std::strcmp(att->name, "g_econst") == 0 && att->data != nullptr &&
              datalistlen(att->data) == 1) {
            NCConstant* c = datalistith(att->data, 0);
            saw_gatt = (c->nctype == NC_ECONST &&
                        c->value.enumv->typ.econst->value.int32v == 10);
          }
        }
        for (unsigned long i = 0; i < listlength(attdefs); ++i) {
          auto* att = static_cast<Symbol*>(listget(attdefs, i));
          if (std::strcmp(att->name, "g2_vatt") == 0 && att->data != nullptr &&
              datalistlen(att->data) == 1) {
            NCConstant* c = datalistith(att->data, 0);
            saw_g2_vatt = (c->nctype == NC_ECONST &&
                           c->value.enumv->typ.econst->value.int32v == 77);
          }
        }
        return saw_gatt && saw_g2_vatt;
      }));
}

TEST(SemanticsTest, ClassicModeConversionAndAttributesInference) {
  // Exercise usingclassic == 1 with CDF-5 keywords so int64 variable converts
  // to NC_INT in processvars() and inferattributetype covers classic types.
  constexpr const char* kClassicCdl = R"(
netcdf classic_mode {
dimensions:
  u = UNLIMITED;
  d = 2;
variables:
  int v_unlim0(u);
  int64 v_i64(d);
  double v_dbl;
  :g_classic_int = 1, 2;
  v_i64:v_att = 10;
  v_dbl:d_att = 2.5;
}
)";
  ParseCdlOptions classic_opts{kClassicCdl, NC_FORMAT_64BIT_DATA, 1, L_BINARY};
  ASSERT_TRUE(RunCdlSemanticsInChild(
      classic_opts, []() { return true; },
      []() {
        Symbol* vi64 = nullptr;
        Symbol* vdbl = nullptr;
        for (unsigned long i = 0; i < listlength(vardefs); ++i) {
          auto* v = static_cast<Symbol*>(listget(vardefs, i));
          if (std::strcmp(v->name, "v_i64") == 0) vi64 = v;
          if (std::strcmp(v->name, "v_dbl") == 0) vdbl = v;
        }
        if (!vi64 || !vdbl) return false;
        if (vi64->typ.typecode != NC_INT) return false;
        if (vdbl->typ.typecode != NC_DOUBLE) return false;
        if (listlength(vi64->var.attributes) != 1 ||
            listlength(vdbl->var.attributes) != 1) {
          return false;
        }
        return true;
      }));

  // Exercise NetCDF-4 attribute inference across all numeric/textual paths,
  // zero-length char attributes, and valid NIL assignments on NC_STRING.
  constexpr const char* kAttrCdl = R"(
netcdf attr_infer {
variables:
  int v1;
  string v_str;
  string v_str_empty;
  :g_empty_untyped = ;
  char :g_empty_char = ;
  :g_strs = "alpha", "beta";
  :g_flt = 1.5f, 2;
  :g_dbl = 1.5f, 2.5;
  :g_u64_big = 18446744073709551615ULL, 1;
  :g_u64_small_and_s = 10ULL, 10s;
  :g_u64_small_and_ll = 10ULL, -1LL;
  :g_up = 1b, 100s, 200;
  :g_neg_byte = -2b, 3b;
  :g_neg_short = -4s, 5s, 6ub;
  :g_neg_int = -6, 7b, 8;
  :g_down = 100, 1b;
  string :g_nil_str = NIL;
  v1:v_untyped = 1b, 2s;
  char v1:v_empty_char = ;
  string v_str:s_nil = NIL;
data:
  v_str = NIL;
}
)";
  ParseCdlOptions attr_opts{kAttrCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
  ASSERT_TRUE(RunCdlSemanticsInChild(
      attr_opts, []() { return true; },
      []() {
        auto find_gatt = [](const char* name) -> Symbol* {
          for (unsigned long i = 0; i < listlength(gattdefs); ++i) {
            auto* a = static_cast<Symbol*>(listget(gattdefs, i));
            if (std::strcmp(a->name, name) == 0) return a;
          }
          return nullptr;
        };
        auto check = [&](const char* name, nc_type expected) -> bool {
          Symbol* a = find_gatt(name);
          return a != nullptr && a->typ.typecode == expected;
        };
        if (!(check("g_empty_untyped", NC_CHAR) &&
              check("g_empty_char", NC_CHAR) && check("g_strs", NC_CHAR) &&
              check("g_flt", NC_FLOAT) && check("g_dbl", NC_DOUBLE) &&
              check("g_u64_big", NC_UINT64) &&
              check("g_u64_small_and_s", NC_UINT64) &&
              check("g_u64_small_and_ll", NC_INT64) && check("g_up", NC_INT) &&
              check("g_neg_byte", NC_BYTE) && check("g_neg_short", NC_SHORT) &&
              check("g_neg_int", NC_INT) && check("g_down", NC_INT) &&
              check("g_nil_str", NC_STRING))) {
          return false;
        }
        Symbol* v1 = nullptr;
        for (unsigned long i = 0; i < listlength(vardefs); ++i) {
          auto* v = static_cast<Symbol*>(listget(vardefs, i));
          if (std::strcmp(v->name, "v1") == 0) v1 = v;
        }
        if (v1 == nullptr || listlength(v1->var.attributes) != 2) return false;
        auto* a0 = static_cast<Symbol*>(listget(v1->var.attributes, 0));
        auto* a1 = static_cast<Symbol*>(listget(v1->var.attributes, 1));
        return a0->typ.typecode == NC_SHORT && a1->typ.typecode == NC_CHAR &&
               datalistlen(a1->data) == 1;
      }));
}

TEST(SemanticsTest, UnlimitedDimsAndMultiDimVarDataPacking) {
  constexpr const char* kCdl = R"(
netcdf unlim_and_data {
dimensions:
  u0 = UNLIMITED;
  u_m0 = UNLIMITED;
  u_m1 = UNLIMITED;
  u_char = UNLIMITED;
  u_char2 = UNLIMITED;
  u_sub = UNLIMITED;
  f2 = 2;
  f3 = 3;
variables:
  int scalar_v;
  int nodata_v(f2);
  int bounded_2d(f2, f3);
  int u_first_only(u0);
  int fixed_then_unlim(f2, u_sub);
  int multi_unlim_rem(u_m0, f2, u_m1);
  int multi_unlim_exact(u_m0, f2, u_m1);
  int unlim_3d(u0, f2, f3);
  int interior_unlim(f2, u_sub, f2);
  char c_unlim_1d(u_char);
  char c_unlim_rem(u_char2, f2);
data:
  scalar_v = 42;
  bounded_2d = 1, 2, 3, 4;
  u_first_only = 10, 20, 30;
  fixed_then_unlim = {1, 2, 3}, {4};
  multi_unlim_rem = {1, 2}, {3}, {4, 5};
  multi_unlim_exact = {10}, {20};
  unlim_3d = 1, 2, 3, 4, 5, 6, 7;
  interior_unlim = {1, 2, 3, 4}, {5, 6};
  c_unlim_1d = 'a', 2ub, "xyz";
  c_unlim_rem = 'p', 'q', 'r';
}
)";

  ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
  ASSERT_TRUE(RunCdlSemanticsInChild(
      opts, []() { return true; },
      []() {
        auto find_dim = [](const char* name) -> Symbol* {
          for (unsigned long i = 0; i < listlength(dimdefs); ++i) {
            auto* d = static_cast<Symbol*>(listget(dimdefs, i));
            if (std::strcmp(d->name, name) == 0) return d;
          }
          return nullptr;
        };
        Symbol* du0 = find_dim("u0");
        Symbol* dum0 = find_dim("u_m0");
        Symbol* dum1 = find_dim("u_m1");
        Symbol* dchar = find_dim("u_char");
        Symbol* dchar2 = find_dim("u_char2");
        Symbol* dsub = find_dim("u_sub");
        if (!du0 || !dum0 || !dum1 || !dchar || !dchar2 || !dsub) return false;
        // Max sizes across variables sharing each unlimited dimension:
        // u0: max(3, ceil(7/6)=2) = 3.
        // u_m0: max(ceil(3/2)=2, ceil(2/2)=1) = 2.
        // u_m1: max(2, 1) = 2.
        // u_char: 1 ('a') + 1 (2ub) + 3 ("xyz") = 5.
        // u_char2: ceil(3/2) = 2.
        // u_sub: max(3, 1, 2, 1) = 3.
        return du0->dim.declsize == size_t{3} &&
               dum0->dim.declsize == size_t{2} &&
               dum1->dim.declsize == size_t{2} &&
               dchar->dim.declsize == size_t{5} &&
               dchar2->dim.declsize == size_t{2} &&
               dsub->dim.declsize == size_t{3};
      }));
}

TEST(SemanticsTest, DirectSemanticEdgeCasesInChild) {
  constexpr const char* kCdl = R"(
netcdf direct_edges {
dimensions:
  u_warn = UNLIMITED;
variables:
  int v_fill;
  char c_warn(u_warn);
  :g_charcon = "placeholder";
}
)";

  char cdl_name[] = "direct_edges.cdl";
  ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY, nullptr, cdl_name};
  ASSERT_TRUE(RunCdlSemanticsInChild(
      opts,
      []() {
        // 1. Add a fill-value attribute with asym->data == NULL and clear
        // v_fill->var.special._Fillvalue so processattributes() hits
        // getfiller().
        Symbol* v_fill = nullptr;
        Symbol* c_warn = nullptr;
        for (unsigned long i = 0; i < listlength(vardefs); ++i) {
          auto* v = static_cast<Symbol*>(listget(vardefs, i));
          if (std::strcmp(v->name, "v_fill") == 0) v_fill = v;
          if (std::strcmp(v->name, "c_warn") == 0) c_warn = v;
        }
        if (v_fill == nullptr || c_warn == nullptr) return false;
        v_fill->var.special._Fillvalue = nullptr;

        Symbol* fill_asym = install(NC_FillValue);
        fill_asym->objectclass = NC_ATT;
        fill_asym->att.var = v_fill;
        fill_asym->data = nullptr;
        fill_asym->container = rootgroup;
        listpush(rootgroup->subnodes, fill_asym);
        listpush(attdefs, fill_asym);

        // 2. Add a typed global attribute with data == NULL to hit the
        // asym->data != NULL short-circuit in processattributes().
        Symbol* g_nodata = install("g_typed_nodata");
        g_nodata->objectclass = NC_ATT;
        g_nodata->att.var = nullptr;
        g_nodata->typ.basetype = primsymbols[NC_INT];
        g_nodata->data = nullptr;
        g_nodata->container = rootgroup;
        listpush(rootgroup->subnodes, g_nodata);
        listpush(gattdefs, g_nodata);

        // 3. Add an attribute in attdefs with asym->att.var == NULL to cover
        // `if (asym->att.var == NULL) continue;` in processattributes().
        Symbol* orphan_att = install("orphan_att");
        orphan_att->objectclass = NC_ATT;
        orphan_att->att.var = nullptr;
        orphan_att->typ.basetype = primsymbols[NC_INT];
        orphan_att->data = builddatalist(1);
        NCConstant* ok_int = nullconst();
        ok_int->nctype = NC_INT;
        ok_int->value.int32v = 7;
        dlappend(orphan_att->data, ok_int);
        orphan_att->container = rootgroup;
        listpush(rootgroup->subnodes, orphan_att);
        listpush(attdefs, orphan_att);

        // 4. Give :g_charcon both NC_CHAR and NC_STRING constants to hit
        // inferattributetype1() charcount++.
        Symbol* gchar = nullptr;
        for (unsigned long i = 0; i < listlength(gattdefs); ++i) {
          auto* a = static_cast<Symbol*>(listget(gattdefs, i));
          if (std::strcmp(a->name, "g_charcon") == 0) gchar = a;
        }
        if (gchar == nullptr) return false;
        reclaimdatalist(gchar->data);
        gchar->data = builddatalist(2);
        NCConstant* ch_c = nullconst();
        ch_c->nctype = NC_CHAR;
        ch_c->value.charv = 'z';
        NCConstant* str_c = nullconst();
        str_c->nctype = NC_STRING;
        str_c->value.stringv.stringv = chkstrdup("hi");
        str_c->value.stringv.len = 2;
        dlappend(gchar->data, ch_c);
        dlappend(gchar->data, str_c);

        // 5. Populate c_warn(u_warn) with NC_CHAR, NC_COMPOUND(NC_DIM),
        // NC_COMPOUND(NC_NAT), and invalid NC_INT to cover all branches +
        // semwarn() diagnostics in computeunlimitedsizes().
        Datalist* cw = builddatalist(4);
        NCConstant* cw_ch = nullconst();
        cw_ch->nctype = NC_CHAR;
        cw_ch->value.charv = 'k';

        Datalist* sub1 = builddatalist(1);
        NCConstant* sub1_v = nullconst();
        sub1_v->nctype = NC_INT;
        sub1_v->value.int32v = 1;
        dlappend(sub1, sub1_v);
        NCConstant* cw_dim = list2const(sub1);
        setsubtype(cw_dim, NC_DIM);

        Datalist* sub2 = builddatalist(1);
        NCConstant* sub2_v = nullconst();
        sub2_v->nctype = NC_INT;
        sub2_v->value.int32v = 2;
        dlappend(sub2, sub2_v);
        NCConstant* cw_non_dim = list2const(sub2);
        setsubtype(cw_non_dim, NC_NAT);

        NCConstant* cw_bad_int = nullconst();
        cw_bad_int->nctype = NC_INT;
        cw_bad_int->value.int32v = 99;

        dlappend(cw, cw_ch);
        dlappend(cw, cw_dim);
        dlappend(cw, cw_non_dim);
        dlappend(cw, cw_bad_int);
        c_warn->data = cw;
        return true;
      },
      []() {
        Symbol* fill_asym = nullptr;
        for (unsigned long i = 0; i < listlength(attdefs); ++i) {
          auto* a = static_cast<Symbol*>(listget(attdefs, i));
          if (std::strcmp(a->name, NC_FillValue) == 0) fill_asym = a;
        }
        Symbol* gchar = nullptr;
        Symbol* g_nodata = nullptr;
        for (unsigned long i = 0; i < listlength(gattdefs); ++i) {
          auto* a = static_cast<Symbol*>(listget(gattdefs, i));
          if (std::strcmp(a->name, "g_charcon") == 0) gchar = a;
          if (std::strcmp(a->name, "g_typed_nodata") == 0) g_nodata = a;
        }
        const bool ok = fill_asym != nullptr && fill_asym->data != nullptr &&
                        fill_asym->typ.typecode == NC_INT && gchar != nullptr &&
                        gchar->typ.typecode == NC_CHAR && g_nodata != nullptr &&
                        g_nodata->typ.typecode == NC_INT &&
                        g_nodata->data == nullptr;
        // getfiller() returns primsymbols[NC_INT]->typ._Fillvalue by pointer;
        // detach asym->data before cleanup() to prevent double-free.
        if (fill_asym != nullptr) fill_asym->data = nullptr;
        return ok;
      }));
}

TEST(SemanticsTest, SemanticErrorsAbortViaFinalizeJmp) {
  // 1. Circular vlen dependency (processtypes line 349).
  {
    constexpr const char* kCdl = R"(
netcdf err_circ {
types:
  int(*) v_circ;
}
)";
    ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
    EXPECT_TRUE(ExpectSemerrorInChild(opts, []() {
      for (unsigned long i = 0; i < listlength(typdefs); ++i) {
        auto* t = static_cast<Symbol*>(listget(typdefs, i));
        if (std::strcmp(t->name, "v_circ") == 0) {
          t->typ.basetype = t;
          return true;
        }
      }
      return false;
    }));
  }

  // 2a. Undefined unprefixed enum constant (findeconstenum line 573).
  {
    constexpr const char* kCdl = R"(
netcdf err_enum_unpref {
types:
  int enum e_t { E_OK = 1 };
variables:
  e_t v;
data:
  v = NO_SUCH_ECONST;
}
)";
    ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
    EXPECT_TRUE(ExpectSemerrorInChild(opts, []() { return true; }));
  }

  // 2b. Undefined constant inside an existing prefixed enum type.
  {
    constexpr const char* kCdl = R"(
netcdf err_enum_pref {
types:
  int enum e_t { E_OK = 1 };
variables:
  e_t v;
data:
  v = PLACEHOLDER;
}
)";
    ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
    EXPECT_TRUE(ExpectSemerrorInChild(opts, []() {
      for (unsigned long i = 0; i < listlength(vardefs); ++i) {
        auto* v = static_cast<Symbol*>(listget(vardefs, i));
        if (std::strcmp(v->name, "v") == 0 && v->data != nullptr &&
            datalistlen(v->data) == 1) {
          NCConstant* con = datalistith(v->data, 0);
          efree(con->value.enumv->name);
          con->value.enumv->name = chkstrdup("e_t.NO_SUCH");
          return true;
        }
      }
      return false;
    }));
  }

  // 3. UNLIMITED not in first dimension under usingclassic == 1 (line 687).
  {
    constexpr const char* kCdl = R"(
netcdf err_classic_unlim {
dimensions:
  f2 = 2;
  u = UNLIMITED;
variables:
  int v(f2, u);
}
)";
    ParseCdlOptions opts{kCdl, NC_FORMAT_CLASSIC, 1, L_BINARY};
    EXPECT_TRUE(ExpectSemerrorInChild(opts, []() { return true; }));
  }

  // 4. Empty datalist assigned to non-char global attribute (line 723).
  {
    constexpr const char* kCdl = R"(
netcdf err_empty_gatt {
  int :g_empty = ;
}
)";
    ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
    EXPECT_TRUE(ExpectSemerrorInChild(opts, []() { return true; }));
  }

  // 5. Empty datalist assigned to non-char variable attribute (line 756).
  {
    constexpr const char* kCdl = R"(
netcdf err_empty_vatt {
variables:
  int v;
  int v:v_empty = ;
}
)";
    ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
    EXPECT_TRUE(ExpectSemerrorInChild(opts, []() { return true; }));
  }

  // 6a. Nested untyped attribute list (> NC_MAX_ATOMIC_TYPE, line 896).
  {
    constexpr const char* kCdl = R"(
netcdf err_nested_att {
  :g_nested = {1, 2};
}
)";
    ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
    EXPECT_TRUE(ExpectSemerrorInChild(opts, []() { return true; }));
  }

  // 6b. Mixed textual + numeric untyped attribute list (line 834 -> 896).
  {
    constexpr const char* kCdl = R"(
netcdf err_mixed_att {
  :g_mixed = "text", 1;
}
)";
    ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
    EXPECT_TRUE(ExpectSemerrorInChild(opts, []() { return true; }));
  }

  // 6c. NULL entry inside untyped attribute datalist (line 817 -> 896).
  {
    constexpr const char* kCdl = R"(
netcdf err_nullcon_att {
  :g_null = 1;
}
)";
    ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
    EXPECT_TRUE(ExpectSemerrorInChild(opts, []() {
      auto* a = static_cast<Symbol*>(listget(gattdefs, 0));
      reclaimconstant(a->data->data[0]);
      a->data->data[0] = nullptr;
      return true;
    }));
  }

  // 7a. NIL on non-string global attribute (validateNILr line 928).
  {
    constexpr const char* kCdl = R"(
netcdf err_nil_gatt {
  int :g_nil = NIL;
}
)";
    ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
    EXPECT_TRUE(ExpectSemerrorInChild(opts, []() { return true; }));
  }

  // 7b. NIL nested inside compound variable data (validateNILr recursion).
  {
    constexpr const char* kCdl = R"(
netcdf err_nil_nested {
types:
  compound c_t { int a; string b; };
variables:
  c_t v;
data:
  v = {1, NIL};
}
)";
    ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
    EXPECT_TRUE(ExpectSemerrorInChild(opts, []() { return true; }));
  }

  // 8. Non-compound data for secondary unlimited dimension under !lastunlim
  //    (computeunlimitedsizes line 1097).
  {
    constexpr const char* kCdl = R"(
netcdf err_multi_unlim_flat {
dimensions:
  u0 = UNLIMITED;
  f2 = 2;
  u1 = UNLIMITED;
variables:
  int v(u0, f2, u1);
data:
  v = 1, 2;
}
)";
    ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
    EXPECT_TRUE(ExpectSemerrorInChild(opts, []() { return true; }));
  }

  // 9. Non-compound data for non-first unlimited dimension
  // (processunlimiteddims
  //    line 1170).
  {
    constexpr const char* kCdl = R"(
netcdf err_second_unlim_flat {
dimensions:
  f2 = 2;
  u0 = UNLIMITED;
variables:
  int v(f2, u0);
data:
  v = 1, 2;
}
)";
    ParseCdlOptions opts{kCdl, NC_FORMAT_NETCDF4, 0, L_BINARY};
    EXPECT_TRUE(ExpectSemerrorInChild(opts, []() { return true; }));
  }
}

}  // namespace
}  // namespace netcdf
