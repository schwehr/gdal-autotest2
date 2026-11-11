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

// Unit tests for //third_party/netcdf/ncgen/getfill.c.
//
// Covers nc_dfaltfillname, nc_getfill across primitive/string/opaque/enum
// types and error paths, and getfiller across variables, primitive caching,
// enums, opaques, vlens, and compounds with scalar, nested, and multi-dim
// bounded/unlimited fields.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/getfill.c

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"

extern "C" {
void freeSymbol(Symbol* sym);
}  // extern "C"

namespace netcdf {
namespace {

struct SymbolDeleter {
  void operator()(Symbol* sym) const { freeSymbol(sym); }
};
using ScopedSymbol = std::unique_ptr<Symbol, SymbolDeleter>;

class SymbolPool {
 public:
  Symbol* MakePrim(nc_type typecode, const char* name) {
    auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
    sym->objectclass = NC_TYPE;
    sym->subclass = NC_PRIM;
    sym->name = estrdup(name);
    sym->typ.typecode = typecode;
    sym->typ.basetype = sym;
    syms_.emplace_back(sym);
    return sym;
  }

  Symbol* MakeDim(const char* name, size_t declsize) {
    auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
    sym->objectclass = NC_DIM;
    sym->name = estrdup(name);
    sym->dim.declsize = declsize;
    sym->dim.isunlimited = (declsize == 0) ? 1 : 0;
    syms_.emplace_back(sym);
    return sym;
  }

  Symbol* MakeType(nc_class subclass, nc_type typecode, const char* name,
                   Symbol* basetype = nullptr) {
    auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
    sym->objectclass = NC_TYPE;
    sym->subclass = subclass;
    sym->name = estrdup(name);
    sym->typ.typecode = typecode;
    sym->typ.basetype = basetype;
    if (subclass == NC_ENUM || subclass == NC_COMPOUND) {
      sym->subnodes = listnew();
    }
    syms_.emplace_back(sym);
    return sym;
  }

  Symbol* MakeField(const char* name, Symbol* basetype,
                    std::initializer_list<Symbol*> dims = {}) {
    auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
    sym->objectclass = NC_TYPE;
    sym->subclass = NC_FIELD;
    sym->name = estrdup(name);
    sym->typ.basetype = basetype;
    sym->typ.typecode = basetype->typ.typecode;
    int idx = 0;
    for (Symbol* dim : dims) {
      sym->typ.dimset.dimsyms[idx++] = dim;
    }
    sym->typ.dimset.ndims = idx;
    syms_.emplace_back(sym);
    return sym;
  }

  Symbol* MakeVar(const char* name, Symbol* basetype) {
    auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
    sym->objectclass = NC_VAR;
    sym->name = estrdup(name);
    sym->typ.basetype = basetype;
    sym->typ.typecode = basetype->typ.typecode;
    syms_.emplace_back(sym);
    return sym;
  }

 private:
  std::vector<ScopedSymbol> syms_;
};

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

class GetfillTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CleanupNcgenBuffers();
    ncgen_shim_reset_globals();
  }

  void TearDown() override { CleanupNcgenBuffers(); }
};

TEST_F(GetfillTest, DfaltFillNameReturnsExpectedConstants) {
  struct NameCase {
    nc_type type;
    const char* expected;
  };
  constexpr NameCase kCases[] = {
      {NC_BYTE, "NC_FILL_BYTE"},     {NC_CHAR, "NC_FILL_CHAR"},
      {NC_SHORT, "NC_FILL_SHORT"},   {NC_INT, "NC_FILL_INT"},
      {NC_FLOAT, "NC_FILL_FLOAT"},   {NC_DOUBLE, "NC_FILL_DOUBLE"},
      {NC_UBYTE, "NC_FILL_UBYTE"},   {NC_USHORT, "NC_FILL_USHORT"},
      {NC_UINT, "NC_FILL_UINT"},     {NC_INT64, "NC_FILL_INT64"},
      {NC_UINT64, "NC_FILL_UINT64"}, {NC_STRING, "NC_FILL_STRING"},
  };
  for (const NameCase& tc : kCases) {
    EXPECT_STREQ(tc.expected, nc_dfaltfillname(tc.type));
  }
}

TEST_F(GetfillTest, NcGetfillPopulatesPrimitivesStringOpaqueAndErrors) {
  NCConstant con{};
  const absl::Cleanup clear_con = [&con] { clearconstant(&con); };

  con = NCConstant{};
  con.nctype = NC_CHAR;
  nc_getfill(&con, nullptr);
  EXPECT_EQ(NC_FILL_CHAR, con.value.charv);

  con = NCConstant{};
  con.nctype = NC_BYTE;
  nc_getfill(&con, nullptr);
  EXPECT_EQ(NC_FILL_BYTE, con.value.int8v);

  con = NCConstant{};
  con.nctype = NC_SHORT;
  nc_getfill(&con, nullptr);
  EXPECT_EQ(NC_FILL_SHORT, con.value.int16v);

  con = NCConstant{};
  con.nctype = NC_INT;
  nc_getfill(&con, nullptr);
  EXPECT_EQ(NC_FILL_INT, con.value.int32v);

  con = NCConstant{};
  con.nctype = NC_FLOAT;
  nc_getfill(&con, nullptr);
  EXPECT_EQ(NC_FILL_FLOAT, con.value.floatv);

  con = NCConstant{};
  con.nctype = NC_DOUBLE;
  nc_getfill(&con, nullptr);
  EXPECT_EQ(NC_FILL_DOUBLE, con.value.doublev);

  con = NCConstant{};
  con.nctype = NC_UBYTE;
  nc_getfill(&con, nullptr);
  EXPECT_EQ(NC_FILL_UBYTE, con.value.uint8v);

  con = NCConstant{};
  con.nctype = NC_USHORT;
  nc_getfill(&con, nullptr);
  EXPECT_EQ(NC_FILL_USHORT, con.value.uint16v);

  con = NCConstant{};
  con.nctype = NC_UINT;
  nc_getfill(&con, nullptr);
  EXPECT_EQ(NC_FILL_UINT, con.value.uint32v);

  con = NCConstant{};
  con.nctype = NC_INT64;
  nc_getfill(&con, nullptr);
  EXPECT_EQ(NC_FILL_INT64, con.value.int64v);

  con = NCConstant{};
  con.nctype = NC_UINT64;
  nc_getfill(&con, nullptr);
  EXPECT_EQ(NC_FILL_UINT64, con.value.uint64v);

  con = NCConstant{};
  con.nctype = NC_STRING;
  nc_getfill(&con, nullptr);
  ASSERT_NE(nullptr, con.value.stringv.stringv);
  EXPECT_STREQ("", con.value.stringv.stringv);
  EXPECT_EQ(1U, con.value.stringv.len);
  clearconstant(&con);

  con = NCConstant{};
  con.nctype = NC_OPAQUE;
  nc_getfill(&con, nullptr);
  ASSERT_NE(nullptr, con.value.opaquev.stringv);
  EXPECT_STREQ("00", con.value.opaquev.stringv);
  EXPECT_EQ(2U, con.value.opaquev.len);
  clearconstant(&con);

  con = NCConstant{};
  con.nctype = NC_NAT;
  nc_getfill(&con, nullptr);
  EXPECT_EQ(1, error_count);
}

TEST_F(GetfillTest, NcGetfillHandlesAllEnumBasetypesAndValidationErrors) {
  SymbolPool pool;
  constexpr nc_type kEnumTypes[] = {
      NC_BYTE,   NC_SHORT, NC_INT,   NC_UBYTE,
      NC_USHORT, NC_UINT,  NC_INT64, NC_UINT64,
  };

  for (const nc_type base : kEnumTypes) {
    Symbol* base_sym = pool.MakePrim(base, "ebase");
    Symbol* enum_sym = pool.MakeType(NC_ENUM, NC_ENUM, "etype", base_sym);
    Symbol* econst = pool.MakeType(NC_ECONST, base, "FIRST", enum_sym);
    NCConstant* ec = nullconst();
    ec->nctype = base;
    ec->value.int64v = 42;
    econst->typ.econst = ec;
    listpush(enum_sym->subnodes, econst);

    NCConstant out{};
    out.nctype = NC_ENUM;
    nc_getfill(&out, enum_sym);
    EXPECT_EQ(42LL, out.value.int64v);
  }

  // Non-enum subclass with non-empty subnodes exercises subclass check safely.
  Symbol* bad_enum = pool.MakeType(NC_COMPOUND, NC_ENUM, "not_enum");
  Symbol* ec_float = pool.MakeType(NC_ECONST, NC_FLOAT, "BAD", bad_enum);
  NCConstant* float_con = nullconst();
  float_con->nctype = NC_FLOAT;
  float_con->value.floatv = 1.5F;
  ec_float->typ.econst = float_con;
  listpush(bad_enum->subnodes, ec_float);

  NCConstant out{};
  out.nctype = NC_ENUM;
  nc_getfill(&out, bad_enum);
  EXPECT_EQ(2, error_count);
}

TEST_F(GetfillTest, GetfillerHandlesVarsPrimitiveCachingVlensAndCompounds) {
  SymbolPool pool;
  Symbol* prim_int = pool.MakePrim(NC_INT, "int");
  Symbol* prim_str = pool.MakePrim(NC_STRING, "string");
  Symbol* opaque_t = pool.MakeType(NC_OPAQUE, NC_OPAQUE, "op_t");

  Symbol* enum_t = pool.MakeType(NC_ENUM, NC_ENUM, "en_t", prim_int);
  Symbol* econst = pool.MakeType(NC_ECONST, NC_INT, "E0", enum_t);
  econst->typ.econst = nullconst();
  econst->typ.econst->nctype = NC_INT;
  econst->typ.econst->value.int32v = 7;
  listpush(enum_t->subnodes, econst);

  // Variable with explicit _FillValue attribute returns it directly.
  Symbol* var_explicit = pool.MakeVar("v_explicit", prim_int);
  Datalist* custom_dl = builddatalist(1);
  NCConstant* custom_con = nullconst();
  custom_con->nctype = NC_INT;
  custom_con->value.int32v = -999;
  dlappend(custom_dl, custom_con);
  var_explicit->var.special._Fillvalue = custom_dl;
  EXPECT_EQ(custom_dl, getfiller(var_explicit));

  // Variable without explicit _FillValue delegates to and caches on basetype.
  Symbol* var_default = pool.MakeVar("v_default", prim_int);
  Datalist* int_fill_1 = getfiller(var_default);
  ASSERT_NE(nullptr, int_fill_1);
  ASSERT_EQ(1U, datalistlen(int_fill_1));
  EXPECT_EQ(NC_FILL_INT, datalistith(int_fill_1, 0)->value.int32v);
  EXPECT_EQ(int_fill_1, getfiller(prim_int));

  Datalist* str_fill = getfiller(prim_str);
  ASSERT_NE(nullptr, str_fill);
  ASSERT_EQ(1U, datalistlen(str_fill));
  EXPECT_STREQ("", datalistith(str_fill, 0)->value.stringv.stringv);

  Datalist* op_fill = getfiller(opaque_t);
  ASSERT_NE(nullptr, op_fill);
  EXPECT_STREQ("00", datalistith(op_fill, 0)->value.opaquev.stringv);

  Datalist* en_fill = getfiller(enum_t);
  ASSERT_NE(nullptr, en_fill);
  EXPECT_EQ(7, datalistith(en_fill, 0)->value.int32v);

  // Nested compound + vlens + 1D/2D bounded/unlimited array fields.
  Symbol* dim2 = pool.MakeDim("d2", 2);
  Symbol* dim3 = pool.MakeDim("d3", 3);
  Symbol* dim_unlim = pool.MakeDim("du", 0);

  Symbol* vlen_int = pool.MakeType(NC_VLEN, NC_VLEN, "vlen_i", prim_int);
  Symbol* vlen_vlen = pool.MakeType(NC_VLEN, NC_VLEN, "vlen_v", vlen_int);
  EXPECT_NE(nullptr, getfiller(vlen_int));
  EXPECT_NE(nullptr, getfiller(vlen_vlen));

  Symbol* inner_cmpd = pool.MakeType(NC_COMPOUND, NC_COMPOUND, "inner_t");
  listpush(inner_cmpd->subnodes, pool.MakeField("inner_i", prim_int));
  listpush(inner_cmpd->subnodes, pool.MakeField("inner_op", opaque_t));

  Symbol* vlen_cmpd = pool.MakeType(NC_VLEN, NC_VLEN, "vlen_c", inner_cmpd);
  EXPECT_NE(nullptr, getfiller(vlen_cmpd));

  Symbol* outer_cmpd = pool.MakeType(NC_COMPOUND, NC_COMPOUND, "outer_t");
  listpush(outer_cmpd->subnodes, pool.MakeField("f_prim", prim_int));
  listpush(outer_cmpd->subnodes, pool.MakeField("f_enum", enum_t));
  listpush(outer_cmpd->subnodes, pool.MakeField("f_op", opaque_t));
  listpush(outer_cmpd->subnodes, pool.MakeField("f_cmpd", inner_cmpd));
  listpush(outer_cmpd->subnodes, pool.MakeField("f_vlen", vlen_int));
  listpush(outer_cmpd->subnodes, pool.MakeField("f_arr1_b", prim_int, {dim3}));
  listpush(outer_cmpd->subnodes,
           pool.MakeField("f_arr1_u", prim_int, {dim_unlim}));
  listpush(outer_cmpd->subnodes,
           pool.MakeField("f_arr2_bu", prim_int, {dim2, dim_unlim}));
  listpush(outer_cmpd->subnodes,
           pool.MakeField("f_arr2_ub", prim_int, {dim_unlim, dim2}));

  Datalist* cmpd_fill = getfiller(outer_cmpd);
  ASSERT_NE(nullptr, cmpd_fill);
  ASSERT_EQ(1U, datalistlen(cmpd_fill));
  Datalist* fields_dl = compoundfor(datalistith(cmpd_fill, 0));
  ASSERT_NE(nullptr, fields_dl);
  EXPECT_EQ(9U, datalistlen(fields_dl));
}

}  // namespace
}  // namespace netcdf
