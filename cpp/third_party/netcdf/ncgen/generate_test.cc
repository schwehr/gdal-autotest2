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

// Unit tests for //third_party/netcdf/ncgen/generate.c.
//
// Exercises generator_getstate(), generator_reset(), generate_attrdata(),
// generate_vardata(), generate_basetype(), pvec(), primitive/enum/opaque/nil
// conversion and opaque length normalization, compound/vlen/field-array
// traversal, whole-var fast-path vs recursive generate_arrayR() with fixed and
// unlimited dimensions, and all semerror() validation paths inside isolated
// forked children.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/generate.c

#include <setjmp.h>

#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
// clang-format off
#include "third_party/netcdf/google/util/ncgen_main_shim.h"
// clang-format on

extern "C" {
void pvec(int rank, size_t* vector);
}  // extern "C"

namespace netcdf {
namespace {

struct DatalistDeleter {
  void operator()(Datalist* dl) const {
    if (dl != nullptr) {
      reclaimdatalist(dl);
    }
  }
};
using UniqueDatalist = std::unique_ptr<Datalist, DatalistDeleter>;

struct BytebufferDeleter {
  void operator()(Bytebuffer* bb) const {
    if (bb != nullptr) {
      bbFree(bb);
    }
  }
};
using UniqueBytebuffer = std::unique_ptr<Bytebuffer, BytebufferDeleter>;

struct SymbolDeleter {
  void operator()(Symbol* sym) const {
    if (sym != nullptr) {
      freeSymbol(sym);
    }
  }
};
using UniqueSymbol = std::unique_ptr<Symbol, SymbolDeleter>;

struct ConstantDeleter {
  void operator()(NCConstant* con) const {
    if (con != nullptr) {
      reclaimconstant(con);
    }
  }
};
using UniqueConstant = std::unique_ptr<NCConstant, ConstantDeleter>;

void FreeNcgenInitBuffers() {
  if (codebuffer != nullptr) {
    bbFree(codebuffer);
    codebuffer = nullptr;
  }
  if (stmt != nullptr) {
    bbFree(stmt);
    stmt = nullptr;
  }
  if (filldatalist != nullptr) {
    filldatalist->readonly = 0;
    freedatalist(filldatalist);
    filldatalist = nullptr;
  }
  cdlname = nullptr;
}

class ScopedNcgenGlobals {
 public:
  ScopedNcgenGlobals() {
    static char kCdlName[] = "generate_test.cdl";
    FreeNcgenInitBuffers();
    ncgen_shim_reset_globals();
    cdlname = kCdlName;
  }

  ~ScopedNcgenGlobals() {
    ncgen_shim_set_finalize_jmp(nullptr, nullptr);
    FreeNcgenInitBuffers();
  }

  ScopedNcgenGlobals(const ScopedNcgenGlobals&) = delete;
  ScopedNcgenGlobals& operator=(const ScopedNcgenGlobals&) = delete;
};

struct RecordedConstant {
  nc_type type = NC_NAT;
  std::string opaque_hex;
  std::string str_val;
  bool has_nonnull_str = false;
  long long i64_val = 0;
  unsigned long long u64_val = 0;
  double f64_val = 0.0;
};

struct RecordedWrite {
  std::string sym_name;
  int rank = 0;
  std::vector<size_t> start;
  std::vector<size_t> count;
  std::string bytes;
};

struct SpyState {
  int next_uid = 0;
  int vlen_list_end_nonnull_ptr_count = 0;
  std::vector<RecordedConstant> constants;
  std::vector<std::string> char_constants;
  std::vector<std::pair<ListClass, size_t>> list_begins;
  std::vector<std::pair<ListClass, size_t>> list_items;
  std::vector<std::pair<ListClass, size_t>> list_ends;
  std::vector<std::pair<int, size_t>> vlen_decls;
  std::vector<std::pair<int, size_t>> vlen_strings;
  std::vector<std::string> vlen_char_contents;
  std::vector<RecordedWrite> writes;
};

SpyState* StateFrom(Generator* gen) {
  void* statep = nullptr;
  generator_getstate(gen, &statep);
  return static_cast<SpyState*>(statep);
}

int SpyCharConstant(Generator* gen, Symbol* /*sym*/, Bytebuffer* codebuf, ...) {
  va_list ap;
  va_start(ap, codebuf);
  Bytebuffer* charbuf = va_arg(ap, Bytebuffer*);
  va_end(ap);

  SpyState* st = StateFrom(gen);
  st->char_constants.emplace_back(bbContents(charbuf), bbLength(charbuf));
  bbCatbuf(codebuf, charbuf);
  return 1;
}

int SpyConstant(Generator* gen, Symbol* /*sym*/, NCConstant* con,
                Bytebuffer* buf, ...) {
  SpyState* st = StateFrom(gen);
  RecordedConstant rc;
  rc.type = con->nctype;
  switch (con->nctype) {
    case NC_OPAQUE:
      if (con->value.opaquev.stringv != nullptr) {
        rc.opaque_hex.assign(con->value.opaquev.stringv,
                             con->value.opaquev.len);
        if (con->value.opaquev.len > 0) {
          bbAppendn(buf, con->value.opaquev.stringv, con->value.opaquev.len);
        }
      }
      break;
    case NC_STRING:
      rc.has_nonnull_str = (con->value.stringv.stringv != nullptr);
      if (con->value.stringv.stringv != nullptr) {
        rc.str_val.assign(con->value.stringv.stringv, con->value.stringv.len);
        if (con->value.stringv.len > 0) {
          bbAppendn(buf, con->value.stringv.stringv, con->value.stringv.len);
        }
      }
      break;
    case NC_CHAR:
      rc.i64_val = static_cast<unsigned char>(con->value.charv);
      bbAppend(buf, con->value.charv);
      break;
    case NC_BYTE:
      rc.i64_val = con->value.int8v;
      bbAppendn(buf, &con->value.int8v, sizeof(con->value.int8v));
      break;
    case NC_SHORT:
      rc.i64_val = con->value.int16v;
      bbAppendn(buf, &con->value.int16v, sizeof(con->value.int16v));
      break;
    case NC_INT:
      rc.i64_val = con->value.int32v;
      bbAppendn(buf, &con->value.int32v, sizeof(con->value.int32v));
      break;
    case NC_UBYTE:
      rc.u64_val = con->value.uint8v;
      bbAppendn(buf, &con->value.uint8v, sizeof(con->value.uint8v));
      break;
    case NC_USHORT:
      rc.u64_val = con->value.uint16v;
      bbAppendn(buf, &con->value.uint16v, sizeof(con->value.uint16v));
      break;
    case NC_UINT:
      rc.u64_val = con->value.uint32v;
      bbAppendn(buf, &con->value.uint32v, sizeof(con->value.uint32v));
      break;
    case NC_INT64:
      rc.i64_val = con->value.int64v;
      bbAppendn(buf, &con->value.int64v, sizeof(con->value.int64v));
      break;
    case NC_UINT64:
      rc.u64_val = con->value.uint64v;
      bbAppendn(buf, &con->value.uint64v, sizeof(con->value.uint64v));
      break;
    case NC_FLOAT:
      rc.f64_val = con->value.floatv;
      bbAppendn(buf, &con->value.floatv, sizeof(con->value.floatv));
      break;
    case NC_DOUBLE:
      rc.f64_val = con->value.doublev;
      bbAppendn(buf, &con->value.doublev, sizeof(con->value.doublev));
      break;
    case NC_ECONST:
      bbAppend(buf, 'E');
      break;
    default:
      break;
  }
  st->constants.push_back(std::move(rc));
  return 1;
}

int SpyListBegin(Generator* gen, Symbol* /*sym*/, void* liststate, ListClass lc,
                 size_t size, Bytebuffer* buf, int* uidp, ...) {
  SpyState* st = StateFrom(gen);
  if (uidp != nullptr) {
    *uidp = ++st->next_uid;
  }
  if (lc == LISTCOMPOUND && liststate != nullptr) {
    *static_cast<size_t*>(liststate) = bbLength(buf);
  }
  st->list_begins.emplace_back(lc, size);
  return 1;
}

int SpyList(Generator* gen, Symbol* /*sym*/, void* /*liststate*/, ListClass lc,
            int /*uid*/, size_t count, Bytebuffer* /*buf*/, ...) {
  SpyState* st = StateFrom(gen);
  st->list_items.emplace_back(lc, count);
  return 1;
}

int SpyListEnd(Generator* gen, Symbol* /*sym*/, void* /*liststate*/,
               ListClass lc, int /*uid*/, size_t count, Bytebuffer* buf, ...) {
  SpyState* st = StateFrom(gen);
  if (lc == LISTVLEN) {
    va_list ap;
    va_start(ap, buf);
    void* vlen_ptr = va_arg(ap, void*);
    va_end(ap);
    if (vlen_ptr != nullptr) {
      ++st->vlen_list_end_nonnull_ptr_count;
    }
  }
  st->list_ends.emplace_back(lc, count);
  return 1;
}

int SpyVlenDecl(Generator* gen, Symbol* /*tsym*/, Bytebuffer* buf, int uid,
                size_t count, ...) {
  va_list ap;
  va_start(ap, count);
  Bytebuffer* vlenbuf = va_arg(ap, Bytebuffer*);
  va_end(ap);

  SpyState* st = StateFrom(gen);
  st->vlen_decls.emplace_back(uid, count);
  if (vlenbuf != nullptr && bbLength(vlenbuf) > 0) {
    bbCatbuf(buf, vlenbuf);
  }
  return 1;
}

int SpyVlenString(Generator* gen, Symbol* /*tsym*/, Bytebuffer* vlenbuf,
                  int* uidp, size_t* sizep, ...) {
  SpyState* st = StateFrom(gen);
  const int uid = ++st->next_uid;
  if (uidp != nullptr) {
    *uidp = uid;
  }
  const size_t sz = bbLength(vlenbuf);
  if (sizep != nullptr) {
    *sizep = sz;
  }
  st->vlen_strings.emplace_back(uid, sz);
  st->vlen_char_contents.emplace_back(bbContents(vlenbuf), sz);
  return 1;
}

int SpyWriter(Generator* gen, Symbol* sym, Bytebuffer* code, int rank,
              const size_t* start, const size_t* count) {
  SpyState* st = StateFrom(gen);
  RecordedWrite w;
  w.sym_name = (sym != nullptr && sym->name != nullptr) ? sym->name : "";
  w.rank = rank;
  if (rank > 0 && start != nullptr) {
    w.start.assign(start, start + rank);
  }
  if (rank > 0 && count != nullptr) {
    w.count.assign(count, count + rank);
  }
  if (code != nullptr && bbLength(code) > 0) {
    w.bytes.assign(bbContents(code), bbLength(code));
  }
  st->writes.push_back(std::move(w));
  return NC_NOERR;
}

Generator MakeSpyGenerator(SpyState* state) {
  Generator gen{};
  gen.globalstate = state;
  gen.charconstant = SpyCharConstant;
  gen.constant = SpyConstant;
  gen.listbegin = SpyListBegin;
  gen.list = SpyList;
  gen.listend = SpyListEnd;
  gen.vlendecl = SpyVlenDecl;
  gen.vlenstring = SpyVlenString;
  return gen;
}

char* DupCString(std::string_view text) {
  auto* out = static_cast<char*>(ecalloc(text.size() + 1));
  if (!text.empty()) {
    std::memcpy(out, text.data(), text.size());
  }
  out[text.size()] = '\0';
  return out;
}

UniqueSymbol MakePrimTypeSymbol(std::string_view name, nc_type typecode) {
  auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
  sym->objectclass = NC_TYPE;
  sym->subclass = NC_PRIM;
  sym->name = DupCString(name);
  sym->typ.typecode = typecode;
  sym->typ.size = ncsize(typecode);
  sym->typ.nelems = 1;
  return UniqueSymbol(sym);
}

UniqueSymbol MakeOpaqueTypeSymbol(std::string_view name, size_t nbytes) {
  auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
  sym->objectclass = NC_TYPE;
  sym->subclass = NC_OPAQUE;
  sym->name = DupCString(name);
  sym->typ.typecode = NC_OPAQUE;
  sym->typ.size = nbytes;
  sym->typ.nelems = 1;
  return UniqueSymbol(sym);
}

UniqueSymbol MakeEnumTypeSymbol(std::string_view name, Symbol* basetype,
                                nc_type effective_typecode) {
  auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
  sym->objectclass = NC_TYPE;
  sym->subclass = NC_ENUM;
  sym->name = DupCString(name);
  sym->typ.basetype = basetype;
  sym->typ.typecode = effective_typecode;
  sym->typ.size = basetype != nullptr ? basetype->typ.size : sizeof(int);
  sym->subnodes = listnew();
  return UniqueSymbol(sym);
}

UniqueSymbol MakeEnumConstSymbol(std::string_view name, Symbol* parent_enum,
                                 int val) {
  auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
  sym->objectclass = NC_TYPE;
  sym->subclass = NC_ECONST;
  sym->name = DupCString(name);
  sym->container = parent_enum;
  sym->typ.basetype =
      parent_enum != nullptr ? parent_enum->typ.basetype : nullptr;
  sym->typ.typecode = NC_INT;
  NCConstant* ec = nullconst();
  ec->nctype = NC_INT;
  ec->lineno = 1;
  ec->value.int32v = val;
  sym->typ.econst = ec;
  if (parent_enum != nullptr && parent_enum->subnodes != nullptr) {
    listpush(parent_enum->subnodes, sym);
  }
  return UniqueSymbol(sym);
}

UniqueSymbol MakeCompoundTypeSymbol(std::string_view name) {
  auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
  sym->objectclass = NC_TYPE;
  sym->subclass = NC_COMPOUND;
  sym->name = DupCString(name);
  sym->typ.typecode = NC_COMPOUND;
  sym->subnodes = listnew();
  return UniqueSymbol(sym);
}

UniqueSymbol MakeFieldSymbol(std::string_view name, Symbol* basetype,
                             const std::vector<Symbol*>& dims) {
  auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
  sym->objectclass = NC_TYPE;
  sym->subclass = NC_FIELD;
  sym->name = DupCString(name);
  sym->typ.basetype = basetype;
  sym->typ.typecode = basetype != nullptr ? basetype->typ.typecode : NC_NAT;
  sym->typ.dimset.ndims = static_cast<int>(dims.size());
  for (size_t i = 0; i < dims.size(); ++i) {
    sym->typ.dimset.dimsyms[i] = dims[i];
  }
  return UniqueSymbol(sym);
}

UniqueSymbol MakeVlenTypeSymbol(std::string_view name, Symbol* basetype) {
  auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
  sym->objectclass = NC_TYPE;
  sym->subclass = NC_VLEN;
  sym->name = DupCString(name);
  sym->typ.basetype = basetype;
  sym->typ.typecode = NC_VLEN;
  return UniqueSymbol(sym);
}

UniqueSymbol MakeDimSymbol(std::string_view name, size_t declsize,
                           bool is_unlimited) {
  auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
  sym->objectclass = NC_DIM;
  sym->name = DupCString(name);
  sym->dim.declsize = declsize;
  sym->dim.isunlimited = is_unlimited ? 1 : 0;
  return UniqueSymbol(sym);
}

UniqueSymbol MakeAttrSymbol(std::string_view name, Symbol* basetype,
                            Datalist* data) {
  auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
  sym->objectclass = NC_ATT;
  sym->name = DupCString(name);
  sym->typ.basetype = basetype;
  sym->typ.typecode = basetype != nullptr ? basetype->typ.typecode : NC_NAT;
  sym->data = data;
  return UniqueSymbol(sym);
}

UniqueSymbol MakeVarSymbol(std::string_view name, Symbol* basetype,
                           const std::vector<Symbol*>& dims, Datalist* data) {
  auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
  sym->objectclass = NC_VAR;
  sym->name = DupCString(name);
  sym->typ.basetype = basetype;
  sym->typ.typecode = basetype != nullptr ? basetype->typ.typecode : NC_NAT;
  sym->typ.dimset.ndims = static_cast<int>(dims.size());
  for (size_t i = 0; i < dims.size(); ++i) {
    sym->typ.dimset.dimsyms[i] = dims[i];
  }
  sym->data = data;
  return UniqueSymbol(sym);
}

NCConstant* MakeConst(nc_type type, int line = 1) {
  NCConstant* con = nullconst();
  con->nctype = type;
  con->lineno = line;
  return con;
}

NCConstant* MakeStringConst(std::string_view s, int line = 1) {
  NCConstant* con = MakeConst(NC_STRING, line);
  con->value.stringv.len = s.size();
  con->value.stringv.stringv = DupCString(s);
  return con;
}

NCConstant* MakeOpaqueConst(std::string_view hex, int line = 1) {
  NCConstant* con = MakeConst(NC_OPAQUE, line);
  con->value.opaquev.len = hex.size();
  con->value.opaquev.stringv = DupCString(hex);
  return con;
}

NCConstant* MakeIntConst(int v, int line = 1) {
  NCConstant* con = MakeConst(NC_INT, line);
  con->value.int32v = v;
  return con;
}

// Keep sigsetjmp in a plain frame with no C++ objects across siglongjmp.
bool CallGenerateBasetypeExpectFinalize(Symbol* tsym, NCConstant* con,
                                        Bytebuffer* codebuf, Datalist* filler,
                                        Generator* generator) {
  sigjmp_buf env;
  int retcode = -1;
  if (sigsetjmp(env, 1) != 0) {
    ncgen_shim_set_finalize_jmp(nullptr, nullptr);
    return retcode == 1;
  }
  ncgen_shim_set_finalize_jmp(&env, &retcode);
  generate_basetype(tsym, con, codebuf, filler, generator);
  ncgen_shim_set_finalize_jmp(nullptr, nullptr);
  return false;
}

TEST(GenerateTest, GeneratorStateResetAndPvec) {
  ScopedNcgenGlobals globals;

  SpyState st{};
  Generator gen = MakeSpyGenerator(nullptr);
  void* out_state = &out_state;
  EXPECT_EQ(1, generator_getstate(&gen, &out_state));
  EXPECT_EQ(nullptr, out_state);

  EXPECT_EQ(1, generator_reset(&gen, &st));
  EXPECT_EQ(1, generator_getstate(&gen, &out_state));
  EXPECT_EQ(&st, out_state);

  // NULL statep must safely be ignored.
  EXPECT_EQ(1, generator_getstate(&gen, nullptr));

  size_t vec[3] = {2, 3, 4};
  pvec(3, vec);
  pvec(0, vec);
}

TEST(GenerateTest, GenerateAttrdataHandlesCharAndPrimitiveAttrs) {
  ScopedNcgenGlobals globals;

  SpyState st{};
  Generator gen = MakeSpyGenerator(&st);
  UniqueSymbol char_t = MakePrimTypeSymbol("char", NC_CHAR);
  UniqueSymbol int_t = MakePrimTypeSymbol("int", NC_INT);

  Datalist* char_dl = builddatalist(1);
  dlappend(char_dl, MakeStringConst("abc"));
  UniqueSymbol char_att = MakeAttrSymbol("note", char_t.get(), char_dl);

  UniqueBytebuffer codebuf(bbNew());
  generate_attrdata(char_att.get(), &gen, SpyWriter, codebuf.get());
  ASSERT_EQ(1u, st.writes.size());
  EXPECT_EQ("note", st.writes[0].sym_name);
  EXPECT_EQ(0, st.writes[0].rank);
  EXPECT_EQ("abc", st.writes[0].bytes);

  bbClear(codebuf.get());
  Datalist* int_dl = builddatalist(2);
  dlappend(int_dl, MakeIntConst(11));
  dlappend(int_dl, MakeIntConst(22));
  UniqueSymbol int_att = MakeAttrSymbol("nums", int_t.get(), int_dl);

  generate_attrdata(int_att.get(), &gen, SpyWriter, codebuf.get());
  ASSERT_EQ(2u, st.writes.size());
  EXPECT_EQ("nums", st.writes[1].sym_name);
  EXPECT_EQ(sizeof(int) * 2, st.writes[1].bytes.size());
  ASSERT_EQ(1u, st.list_begins.size());
  EXPECT_EQ(std::make_pair(LISTATTR, size_t{2}), st.list_begins[0]);
  ASSERT_EQ(2u, st.list_items.size());
  ASSERT_EQ(1u, st.list_ends.size());
  EXPECT_EQ(std::make_pair(LISTATTR, size_t{2}), st.list_ends[0]);
}

TEST(GenerateTest, GenerateVardataCoversNullScalarCharChunkedFastAndRecursive) {
  ScopedNcgenGlobals globals;

  SpyState st{};
  Generator gen = MakeSpyGenerator(&st);
  UniqueSymbol char_t = MakePrimTypeSymbol("char", NC_CHAR);
  UniqueSymbol int_t = MakePrimTypeSymbol("int", NC_INT);
  UniqueSymbol d2 = MakeDimSymbol("d2", 2, /*is_unlimited=*/false);
  UniqueSymbol d3 = MakeDimSymbol("d3", 3, /*is_unlimited=*/false);
  UniqueSymbol d4 = MakeDimSymbol("d4", 4, /*is_unlimited=*/false);
  UniqueSymbol u3 = MakeDimSymbol("u3", 3, /*is_unlimited=*/true);

  UniqueBytebuffer codebuf(bbNew());

  // NULL data returns immediately without calling writer.
  {
    UniqueSymbol nodata_v =
        MakeVarSymbol("nodata", int_t.get(), {d2.get()}, nullptr);
    generate_vardata(nodata_v.get(), &gen, SpyWriter, codebuf.get());
    EXPECT_TRUE(st.writes.empty());
  }

  // Scalar variable (`rank == 0`).
  {
    Datalist* sdl = builddatalist(1);
    dlappend(sdl, MakeIntConst(42));
    UniqueSymbol scalar_v = MakeVarSymbol("scalar_v", int_t.get(), {}, sdl);
    generate_vardata(scalar_v.get(), &gen, SpyWriter, codebuf.get());
    ASSERT_EQ(1u, st.writes.size());
    EXPECT_EQ("scalar_v", st.writes[0].sym_name);
    EXPECT_EQ(0, st.writes[0].rank);
    int out_val = 0;
    std::memcpy(&out_val, st.writes[0].bytes.data(), sizeof(out_val));
    EXPECT_EQ(42, out_val);
  }

  // NC_CHAR 1D variable with NC_CHUNKED and non-NULL _ChunkSizes.
  {
    bbClear(codebuf.get());
    Datalist* cdl = builddatalist(1);
    dlappend(cdl, MakeStringConst("xy"));
    UniqueSymbol cvar = MakeVarSymbol("cvar", char_t.get(), {d4.get()}, cdl);
    cvar->var.special._Storage = NC_CHUNKED;
    cvar->var.special._ChunkSizes =
        static_cast<size_t*>(ecalloc(sizeof(size_t)));
    cvar->var.special._ChunkSizes[0] = 2;

    generate_vardata(cvar.get(), &gen, SpyWriter, codebuf.get());
    ASSERT_EQ(2u, st.writes.size());
    EXPECT_EQ("cvar", st.writes[1].sym_name);
    EXPECT_EQ(1, st.writes[1].rank);
    EXPECT_THAT(st.writes[1].start, ::testing::ElementsAre(size_t{0}));
    EXPECT_THAT(st.writes[1].count, ::testing::ElementsAre(size_t{4}));
    const std::string expected =
        std::string("xy") + std::string(2, NC_FILL_CHAR);
    EXPECT_EQ(expected, st.writes[1].bytes);
    EXPECT_EQ(0u, bbLength(codebuf.get()));
  }

  // Fast whole-var path (`totalsize == 4 <= wholevarsize && nunlimited == 0`)
  // with NC_CHUNKED and _ChunkSizes == NULL, plus sparse datalist hitting
  // `con == NULL => &fillconstant`.
  {
    bbClear(codebuf.get());
    Datalist* dl = builddatalist(2);
    dlappend(dl, MakeIntConst(100));
    dlappend(dl, MakeIntConst(200));
    UniqueSymbol v_fast =
        MakeVarSymbol("v_fast", int_t.get(), {d2.get(), d2.get()}, dl);
    v_fast->var.special._Storage = NC_CHUNKED;
    v_fast->var.special._ChunkSizes = nullptr;

    generate_vardata(v_fast.get(), &gen, SpyWriter, codebuf.get());
    ASSERT_EQ(3u, st.writes.size());
    EXPECT_EQ("v_fast", st.writes[2].sym_name);
    EXPECT_THAT(st.writes[2].start,
                ::testing::ElementsAre(size_t{0}, size_t{0}));
    EXPECT_THAT(st.writes[2].count,
                ::testing::ElementsAre(size_t{2}, size_t{2}));
    int vals[4]{};
    ASSERT_EQ(sizeof(vals), st.writes[2].bytes.size());
    std::memcpy(vals, st.writes[2].bytes.data(), sizeof(vals));
    EXPECT_THAT(vals,
                ::testing::ElementsAre(100, 200, NC_FILL_INT, NC_FILL_INT));
  }

  // Recursive generate_arrayR (`totalsize == 6 > wholevarsize == 4`) with a
  // sparse 1-row datalist hitting `subdata = filldatalist` on row 1.
  {
    bbClear(codebuf.get());
    wholevarsize = 4;
    Datalist* row0 = builddatalist(3);
    dlappend(row0, MakeIntConst(1));
    dlappend(row0, MakeIntConst(2));
    dlappend(row0, MakeIntConst(3));
    Datalist* dl = builddatalist(1);
    dlappend(dl, builddatasublist(row0));

    UniqueSymbol v_rec =
        MakeVarSymbol("v_rec", int_t.get(), {d2.get(), d3.get()}, dl);
    generate_vardata(v_rec.get(), &gen, SpyWriter, codebuf.get());

    ASSERT_EQ(5u, st.writes.size());
    EXPECT_THAT(st.writes[3].start,
                ::testing::ElementsAre(size_t{0}, size_t{0}));
    EXPECT_THAT(st.writes[3].count,
                ::testing::ElementsAre(size_t{1}, size_t{3}));
    int row0_vals[3]{};
    std::memcpy(row0_vals, st.writes[3].bytes.data(), sizeof(row0_vals));
    EXPECT_THAT(row0_vals, ::testing::ElementsAre(1, 2, 3));

    EXPECT_THAT(st.writes[4].start,
                ::testing::ElementsAre(size_t{1}, size_t{0}));
    EXPECT_THAT(st.writes[4].count,
                ::testing::ElementsAre(size_t{1}, size_t{3}));
    int row1_vals[3]{};
    std::memcpy(row1_vals, st.writes[4].bytes.data(), sizeof(row1_vals));
    EXPECT_THAT(row1_vals,
                ::testing::ElementsAre(NC_FILL_INT, NC_FILL_INT, NC_FILL_INT));
  }

  // Recursive generate_arrayR with unlimited trailing dimension `(d2, u3)`.
  {
    bbClear(codebuf.get());
    Datalist* inner0 = builddatalist(3);
    dlappend(inner0, MakeIntConst(11));
    dlappend(inner0, MakeIntConst(12));
    dlappend(inner0, MakeIntConst(13));
    Datalist* outer0 = builddatalist(1);
    dlappend(outer0, builddatasublist(inner0));

    Datalist* inner1 = builddatalist(2);
    dlappend(inner1, MakeIntConst(21));
    dlappend(inner1, MakeIntConst(22));
    Datalist* outer1 = builddatalist(1);
    dlappend(outer1, builddatasublist(inner1));

    Datalist* dl = builddatalist(2);
    dlappend(dl, builddatasublist(outer0));
    dlappend(dl, builddatasublist(outer1));

    UniqueSymbol v_unlim =
        MakeVarSymbol("v_unlim", int_t.get(), {d2.get(), u3.get()}, dl);
    generate_vardata(v_unlim.get(), &gen, SpyWriter, codebuf.get());

    ASSERT_EQ(7u, st.writes.size());
    int r0[3]{};
    int r1[3]{};
    std::memcpy(r0, st.writes[5].bytes.data(), sizeof(r0));
    std::memcpy(r1, st.writes[6].bytes.data(), sizeof(r1));
    EXPECT_THAT(r0, ::testing::ElementsAre(11, 12, 13));
    EXPECT_THAT(r1, ::testing::ElementsAre(21, 22, NC_FILL_INT));
  }

  // Recursive generate_arrayR with 1D unlimited `(u3)` hits `dimindex == 0`
  // with `isunlimited` true.
  {
    bbClear(codebuf.get());
    Datalist* dl_1d = builddatalist(2);
    dlappend(dl_1d, MakeIntConst(101));
    dlappend(dl_1d, MakeIntConst(102));

    UniqueSymbol v_1d_unlim =
        MakeVarSymbol("v_1d_unlim", int_t.get(), {u3.get()}, dl_1d);
    generate_vardata(v_1d_unlim.get(), &gen, SpyWriter, codebuf.get());

    ASSERT_EQ(8u, st.writes.size());
    EXPECT_THAT(st.writes[7].start, ::testing::ElementsAre(size_t{0}));
    EXPECT_THAT(st.writes[7].count, ::testing::ElementsAre(size_t{3}));
    int uvals[3]{};
    ASSERT_EQ(sizeof(uvals), st.writes[7].bytes.size());
    std::memcpy(uvals, st.writes[7].bytes.data(), sizeof(uvals));
    EXPECT_THAT(uvals, ::testing::ElementsAre(101, 102, NC_FILL_INT));
  }
}

TEST(GenerateTest, GeneratePrimdataCoversAllTypesNilOpaqueAndEnum) {
  ScopedNcgenGlobals globals;

  SpyState st{};
  Generator gen = MakeSpyGenerator(&st);
  UniqueBytebuffer codebuf(bbNew());

  UniqueSymbol t_char = MakePrimTypeSymbol("char", NC_CHAR);
  UniqueSymbol t_byte = MakePrimTypeSymbol("byte", NC_BYTE);
  UniqueSymbol t_short = MakePrimTypeSymbol("short", NC_SHORT);
  UniqueSymbol t_int = MakePrimTypeSymbol("int", NC_INT);
  UniqueSymbol t_float = MakePrimTypeSymbol("float", NC_FLOAT);
  UniqueSymbol t_double = MakePrimTypeSymbol("double", NC_DOUBLE);
  UniqueSymbol t_ubyte = MakePrimTypeSymbol("ubyte", NC_UBYTE);
  UniqueSymbol t_ushort = MakePrimTypeSymbol("ushort", NC_USHORT);
  UniqueSymbol t_uint = MakePrimTypeSymbol("uint", NC_UINT);
  UniqueSymbol t_int64 = MakePrimTypeSymbol("int64", NC_INT64);
  UniqueSymbol t_uint64 = MakePrimTypeSymbol("uint64", NC_UINT64);
  UniqueSymbol t_string = MakePrimTypeSymbol("string", NC_STRING);

  struct PrimCase {
    Symbol* type_sym;
    nc_type src_type;
  };
  const PrimCase kPrimCases[] = {
      {t_char.get(), NC_CHAR},     {t_byte.get(), NC_BYTE},
      {t_short.get(), NC_SHORT},   {t_int.get(), NC_INT},
      {t_float.get(), NC_FLOAT},   {t_double.get(), NC_DOUBLE},
      {t_ubyte.get(), NC_UBYTE},   {t_ushort.get(), NC_USHORT},
      {t_uint.get(), NC_UINT},     {t_int64.get(), NC_INT64},
      {t_uint64.get(), NC_UINT64}, {t_string.get(), NC_STRING},
  };

  for (const PrimCase& pc : kPrimCases) {
    UniqueConstant src((pc.src_type == NC_STRING) ? MakeStringConst("hi")
                                                  : MakeConst(pc.src_type));
    if (pc.src_type == NC_CHAR) src->value.charv = 'Q';
    if (pc.src_type == NC_BYTE) src->value.int8v = -5;
    if (pc.src_type == NC_SHORT) src->value.int16v = -123;
    if (pc.src_type == NC_INT) src->value.int32v = 456;
    if (pc.src_type == NC_FLOAT) src->value.floatv = 1.5F;
    if (pc.src_type == NC_DOUBLE) src->value.doublev = 2.25;
    if (pc.src_type == NC_UBYTE) src->value.uint8v = 200U;
    if (pc.src_type == NC_USHORT) src->value.uint16v = 50000U;
    if (pc.src_type == NC_UINT) src->value.uint32v = 3000000000U;
    if (pc.src_type == NC_INT64) src->value.int64v = -9000000000LL;
    if (pc.src_type == NC_UINT64) src->value.uint64v = 18000000000ULL;

    generate_basetype(pc.type_sym, src.get(), codebuf.get(), nullptr, &gen);
  }
  ASSERT_EQ(12u, st.constants.size());
  EXPECT_EQ("hi", st.constants[11].str_val);

  // Primitive fill handling: NULL constant and explicit NC_FILLVALUE with
  // default filler, plus explicit custom primitive filler.
  generate_basetype(t_int.get(), nullptr, codebuf.get(), nullptr, &gen);
  EXPECT_EQ(NC_FILL_INT, st.constants.back().i64_val);

  generate_basetype(t_int.get(), &fillconstant, codebuf.get(), nullptr, &gen);
  EXPECT_EQ(NC_FILL_INT, st.constants.back().i64_val);

  UniqueDatalist custom_int_fill(builddatalist(1));
  dlappend(custom_int_fill.get(), MakeIntConst(-777));
  generate_basetype(t_int.get(), &fillconstant, codebuf.get(),
                    custom_int_fill.get(), &gen);
  EXPECT_EQ(-777, st.constants.back().i64_val);

  // NC_NIL -> NC_STRING produces empty NULL string.
  UniqueConstant nil_con(MakeConst(NC_NIL));
  generate_basetype(t_string.get(), nil_con.get(), codebuf.get(), nullptr,
                    &gen);
  EXPECT_FALSE(st.constants.back().has_nonnull_str);

  // Opaque normalization: exact (6 nibbles), truncated (10->6), expanded
  // (2->6).
  UniqueSymbol opq3 = MakeOpaqueTypeSymbol("opq3_t", 3);
  UniqueConstant opq_exact(MakeOpaqueConst("a1b2c3"));
  UniqueConstant opq_long(MakeOpaqueConst("0102030405"));
  UniqueConstant opq_short(MakeOpaqueConst("ab"));
  generate_basetype(opq3.get(), opq_exact.get(), codebuf.get(), nullptr, &gen);
  generate_basetype(opq3.get(), opq_long.get(), codebuf.get(), nullptr, &gen);
  generate_basetype(opq3.get(), opq_short.get(), codebuf.get(), nullptr, &gen);
  const size_t n = st.constants.size();
  EXPECT_EQ("a1b2c3", st.constants[n - 3].opaque_hex);
  EXPECT_EQ("010203", st.constants[n - 2].opaque_hex);
  EXPECT_EQ("ab0000", st.constants[n - 1].opaque_hex);

  // Valid NC_OPAQUE fill (`con == NULL`) normalizes default "00" to 6 nibbles.
  generate_basetype(opq3.get(), nullptr, codebuf.get(), nullptr, &gen);
  EXPECT_EQ("000000", st.constants.back().opaque_hex);

  // NC_OPAQUE -> NC_INT numeric conversion (>= 8 hex nibbles = >= 4 bytes).
  UniqueConstant opq_num(MakeOpaqueConst("01020304"));
  generate_basetype(t_int.get(), opq_num.get(), codebuf.get(), nullptr, &gen);
  EXPECT_EQ(NC_INT, st.constants.back().type);

  // Valid NC_ENUM with NC_ECONST constant + explicit enum fill, and direct
  // NC_ECONST typecode branch (`target->nctype == NC_ECONST && subclass ==
  // NC_ENUM`).
  UniqueSymbol enum_t = MakeEnumTypeSymbol("mode_t", t_int.get(), NC_INT);
  UniqueSymbol ec0 = MakeEnumConstSymbol("MODE_OFF", enum_t.get(), 0);
  UniqueSymbol ec1 = MakeEnumConstSymbol("MODE_ON", enum_t.get(), 7);
  UniqueConstant ec_con(MakeConst(NC_ECONST));
  ec_con->value.enumv = ec1.get();
  generate_basetype(enum_t.get(), ec_con.get(), codebuf.get(), nullptr, &gen);
  EXPECT_EQ(7, st.constants.back().i64_val);

  UniqueDatalist enum_fill(builddatalist(1));
  NCConstant* ec0_con = MakeConst(NC_ECONST);
  ec0_con->value.enumv = ec0.get();
  dlappend(enum_fill.get(), ec0_con);
  generate_basetype(enum_t.get(), nullptr, codebuf.get(), enum_fill.get(),
                    &gen);
  EXPECT_EQ(0, st.constants.back().i64_val);

  UniqueSymbol enum_raw = MakeEnumTypeSymbol("raw_t", t_int.get(), NC_ECONST);
  UniqueSymbol ec_raw = MakeEnumConstSymbol("RAW_0", enum_raw.get(), 3);
  UniqueConstant ec_raw_con(MakeConst(NC_ECONST));
  ec_raw_con->value.enumv = ec_raw.get();
  generate_basetype(enum_raw.get(), ec_raw_con.get(), codebuf.get(), nullptr,
                    &gen);
  EXPECT_EQ(NC_ECONST, st.constants.back().type);
}

TEST(GenerateTest, GenerateBasetypeCompoundVlenAndFieldArrays) {
  ScopedNcgenGlobals globals;

  SpyState st{};
  Generator gen = MakeSpyGenerator(&st);
  UniqueBytebuffer codebuf(bbNew());

  UniqueSymbol t_int = MakePrimTypeSymbol("int", NC_INT);
  UniqueSymbol t_char = MakePrimTypeSymbol("char", NC_CHAR);
  UniqueSymbol d2 = MakeDimSymbol("d2", 2, /*is_unlimited=*/false);
  UniqueSymbol d4 = MakeDimSymbol("d4", 4, /*is_unlimited=*/false);

  // Compound type with: scalar int field, int[2] field, char[4] field.
  UniqueSymbol cmpd_t = MakeCompoundTypeSymbol("rec_t");
  UniqueSymbol f_id = MakeFieldSymbol("id", t_int.get(), {});
  UniqueSymbol f_vec = MakeFieldSymbol("vec", t_int.get(), {d2.get()});
  UniqueSymbol f_tag = MakeFieldSymbol("tag", t_char.get(), {d4.get()});
  listpush(cmpd_t->subnodes, f_id.get());
  listpush(cmpd_t->subnodes, f_vec.get());
  listpush(cmpd_t->subnodes, f_tag.get());

  // 1) Full compound instance.
  Datalist* vec_dl = builddatalist(2);
  dlappend(vec_dl, MakeIntConst(5));
  dlappend(vec_dl, MakeIntConst(6));
  Datalist* tag_dl = builddatalist(1);
  dlappend(tag_dl, MakeStringConst("OK"));
  Datalist* cmpd_dl = builddatalist(3);
  dlappend(cmpd_dl, MakeIntConst(9));
  dlappend(cmpd_dl, builddatasublist(vec_dl));
  dlappend(cmpd_dl, builddatasublist(tag_dl));
  UniqueDatalist full_holder(builddatalist(1));
  dlappend(full_holder.get(), builddatasublist(cmpd_dl));

  generate_basetype(cmpd_t.get(), datalistith(full_holder.get(), 0),
                    codebuf.get(), nullptr, &gen);
  const std::string expected_tag =
      std::string("OK") + std::string(2, NC_FILL_CHAR);
  ASSERT_FALSE(st.char_constants.empty());
  EXPECT_EQ(expected_tag, st.char_constants.back());

  // 2) NULL compound triggers getfiller() + compound fill path.
  generate_basetype(cmpd_t.get(), nullptr, codebuf.get(), nullptr, &gen);

  // 3) NC_FILLVALUE triggers fill path too.
  generate_basetype(cmpd_t.get(), &fillconstant, codebuf.get(), nullptr, &gen);

  // 3b) Partial compound (`{ 77 }` omitting array fields) leaves con == NULL
  // in generate_fieldarray(), hitting `data = NULL`, `data == NULL ? NULL`,
  // and gen_chararray(..., data=NULL).
  {
    const size_t consts_before = st.constants.size();
    const size_t chars_before = st.char_constants.size();
    Datalist* partial_dl = builddatalist(1);
    dlappend(partial_dl, MakeIntConst(77));
    UniqueDatalist partial_holder(builddatalist(1));
    dlappend(partial_holder.get(), builddatasublist(partial_dl));

    generate_basetype(cmpd_t.get(), datalistith(partial_holder.get(), 0),
                      codebuf.get(), nullptr, &gen);
    ASSERT_EQ(consts_before + 3, st.constants.size());
    EXPECT_EQ(77, st.constants[consts_before].i64_val);
    EXPECT_EQ(NC_FILL_INT, st.constants[consts_before + 1].i64_val);
    EXPECT_EQ(NC_FILL_INT, st.constants[consts_before + 2].i64_val);
    ASSERT_EQ(chars_before + 1, st.char_constants.size());
    EXPECT_TRUE(st.char_constants.back().empty());
  }

  // 3c) Compound with explicit NC_FILLVALUE placeholders for array fields
  // hits `con != NULL && isfillconst(con)` in generate_basetype() &
  // generate_fieldarray().
  {
    const size_t consts_before = st.constants.size();
    Datalist* fill_fields_dl = builddatalist(3);
    dlappend(fill_fields_dl, MakeIntConst(88));
    dlappend(fill_fields_dl, cloneconstant(&fillconstant));
    dlappend(fill_fields_dl, cloneconstant(&fillconstant));
    UniqueDatalist fill_fields_holder(builddatalist(1));
    dlappend(fill_fields_holder.get(), builddatasublist(fill_fields_dl));

    generate_basetype(cmpd_t.get(), datalistith(fill_fields_holder.get(), 0),
                      codebuf.get(), nullptr, &gen);
    ASSERT_EQ(consts_before + 3, st.constants.size());
    EXPECT_EQ(88, st.constants[consts_before].i64_val);
    EXPECT_EQ(NC_FILL_INT, st.constants[consts_before + 1].i64_val);
    EXPECT_EQ(NC_FILL_INT, st.constants[consts_before + 2].i64_val);
  }

  // 4) Non-char VLEN (`int(*)`) with explicit list + NULL/NC_FILLVALUE fill.
  UniqueSymbol ivlen_t = MakeVlenTypeSymbol("ivlen_t", t_int.get());
  Datalist* vlen_items = builddatalist(2);
  dlappend(vlen_items, MakeIntConst(31));
  dlappend(vlen_items, MakeIntConst(32));
  UniqueDatalist ivlen_holder(builddatalist(1));
  dlappend(ivlen_holder.get(), builddatasublist(vlen_items));

  generate_basetype(ivlen_t.get(), datalistith(ivlen_holder.get(), 0),
                    codebuf.get(), nullptr, &gen);
  generate_basetype(ivlen_t.get(), nullptr, codebuf.get(), nullptr, &gen);
  generate_basetype(ivlen_t.get(), &fillconstant, codebuf.get(), nullptr, &gen);
  EXPECT_EQ(3, st.vlen_list_end_nonnull_ptr_count);

  // 5) Char VLEN (`char(*)`) routes through gen_charseq + vlenstring +
  // vlendecl.
  UniqueSymbol cvlen_t = MakeVlenTypeSymbol("cvlen_t", t_char.get());
  Datalist* cvlen_items = builddatalist(2);
  dlappend(cvlen_items, MakeStringConst("ab"));
  dlappend(cvlen_items, MakeStringConst("cd"));
  UniqueDatalist cvlen_holder(builddatalist(1));
  dlappend(cvlen_holder.get(), builddatasublist(cvlen_items));

  generate_basetype(cvlen_t.get(), datalistith(cvlen_holder.get(), 0),
                    codebuf.get(), nullptr, &gen);
  ASSERT_EQ(1u, st.vlen_char_contents.size());
  EXPECT_EQ("abcd", st.vlen_char_contents[0]);
}

TEST(GenerateTest, SemerrorValidationPathsAbortCleanlyInForkedChildren) {
  auto expect_semerror =
      [](auto&& setup_and_call) -> ::testing::AssertionResult {
    return RunInForkedChild([&]() -> bool {
      ScopedNcgenGlobals globals;
      SpyState st{};
      Generator gen = MakeSpyGenerator(&st);
      UniqueBytebuffer codebuf(bbNew());
      return setup_and_call(&gen, codebuf.get());
    });
  };

  // 1. Expected primitive, found {..}.
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol t_int = MakePrimTypeSymbol("int", NC_INT);
    UniqueDatalist inner(builddatalist(1));
    dlappend(inner.get(), MakeIntConst(1));
    UniqueDatalist wrap(builddatalist(1));
    dlappend(wrap.get(), builddatasublist(inner.release()));
    return CallGenerateBasetypeExpectFinalize(
        t_int.get(), datalistith(wrap.get(), 0), codebuf, nullptr, gen);
  }));

  // 2. Compound fill value not enclosed in {..} (non-null non-list con).
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol cmpd = MakeCompoundTypeSymbol("c_t");
    UniqueDatalist bad_fill(builddatalist(1));
    dlappend(bad_fill.get(), MakeIntConst(5, 10));
    return CallGenerateBasetypeExpectFinalize(cmpd.get(), nullptr, codebuf,
                                              bad_fill.get(), gen);
  }));

  // 3. Compound fill value has NULL fill->data[0].
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol cmpd = MakeCompoundTypeSymbol("c_null_t");
    UniqueDatalist null_elem_fill(builddatalist(1));
    dlappend(null_elem_fill.get(), nullptr);
    return CallGenerateBasetypeExpectFinalize(cmpd.get(), nullptr, codebuf,
                                              null_elem_fill.get(), gen);
  }));

  // 4. Compound value must be enclosed in {..}.
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol cmpd = MakeCompoundTypeSymbol("c2_t");
    UniqueConstant bad_con(MakeIntConst(7, 11));
    return CallGenerateBasetypeExpectFinalize(cmpd.get(), bad_con.get(),
                                              codebuf, nullptr, gen);
  }));

  // 5. Compound datalist longer than number of fields.
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol t_int = MakePrimTypeSymbol("int", NC_INT);
    UniqueSymbol cmpd = MakeCompoundTypeSymbol("c_short_t");
    UniqueSymbol f0 = MakeFieldSymbol("f0", t_int.get(), {});
    listpush(cmpd->subnodes, f0.get());
    Datalist* dl = builddatalist(2);
    dlappend(dl, MakeIntConst(1));
    dlappend(dl, MakeIntConst(2));
    UniqueDatalist holder(builddatalist(1));
    dlappend(holder.get(), builddatasublist(dl));
    return CallGenerateBasetypeExpectFinalize(
        cmpd.get(), datalistith(holder.get(), 0), codebuf, nullptr, gen);
  }));

  // 6. Vlen fill value not enclosed in {..}.
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol t_int = MakePrimTypeSymbol("int", NC_INT);
    UniqueSymbol vlen = MakeVlenTypeSymbol("v_t", t_int.get());
    UniqueDatalist bad_fill(builddatalist(1));
    dlappend(bad_fill.get(), MakeIntConst(3, 12));
    return CallGenerateBasetypeExpectFinalize(vlen.get(), nullptr, codebuf,
                                              bad_fill.get(), gen);
  }));

  // 7. Vlen value must be enclosed in {..}.
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol t_int = MakePrimTypeSymbol("int", NC_INT);
    UniqueSymbol vlen = MakeVlenTypeSymbol("v2_t", t_int.get());
    UniqueConstant bad_con(MakeIntConst(9, 13));
    return CallGenerateBasetypeExpectFinalize(vlen.get(), bad_con.get(),
                                              codebuf, nullptr, gen);
  }));

  // 8. Dimensioned NC_FIELD without {...}.
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol t_int = MakePrimTypeSymbol("int", NC_INT);
    UniqueSymbol d2 = MakeDimSymbol("d2", 2, /*is_unlimited=*/false);
    UniqueSymbol fld = MakeFieldSymbol("arr_f", t_int.get(), {d2.get()});
    UniqueConstant bad_con(MakeIntConst(4, 14));
    return CallGenerateBasetypeExpectFinalize(fld.get(), bad_con.get(), codebuf,
                                              nullptr, gen);
  }));

  // 9. NC_NIL assigned to non-NC_STRING type (`NC_INT`).
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol t_int = MakePrimTypeSymbol("int", NC_INT);
    UniqueConstant nil_con(MakeConst(NC_NIL, 15));
    return CallGenerateBasetypeExpectFinalize(t_int.get(), nil_con.get(),
                                              codebuf, nullptr, gen);
  }));

  // 10. NC_OPAQUE assigned to non-numeric/non-opaque type (`NC_ENUM`).
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol t_int = MakePrimTypeSymbol("int", NC_INT);
    UniqueSymbol enum_t = MakeEnumTypeSymbol("en_t", t_int.get(), NC_INT);
    UniqueConstant opq(MakeOpaqueConst("abcd", 16));
    return CallGenerateBasetypeExpectFinalize(enum_t.get(), opq.get(), codebuf,
                                              nullptr, gen);
  }));

  // 11. NC_ECONST belonging to a different enum type.
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol t_int = MakePrimTypeSymbol("int", NC_INT);
    UniqueSymbol enum_a = MakeEnumTypeSymbol("en_a", t_int.get(), NC_INT);
    UniqueSymbol enum_b = MakeEnumTypeSymbol("en_b", t_int.get(), NC_INT);
    UniqueSymbol ec_a = MakeEnumConstSymbol("A0", enum_a.get(), 1);
    UniqueConstant ec_con(MakeConst(NC_ECONST, 17));
    ec_con->value.enumv = ec_a.get();
    return CallGenerateBasetypeExpectFinalize(enum_b.get(), ec_con.get(),
                                              codebuf, nullptr, gen);
  }));

  // 12. Unknown primitive constant (`NC_NAT`) falls to default `match = 0`.
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol t_int = MakePrimTypeSymbol("int", NC_INT);
    UniqueConstant nat_con(MakeConst(NC_NAT, 18));
    return CallGenerateBasetypeExpectFinalize(t_int.get(), nat_con.get(),
                                              codebuf, nullptr, gen);
  }));

  // 13. Target nctype == NC_ECONST when basetype->subclass != NC_ENUM.
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol bogus_prim = MakePrimTypeSymbol("bogus_econst", NC_ECONST);
    UniqueConstant int_con(MakeIntConst(3, 19));
    return CallGenerateBasetypeExpectFinalize(bogus_prim.get(), int_con.get(),
                                              codebuf, nullptr, gen);
  }));

  // 14. Passing filldatalist as explicit filler lets fillconstant reach
  // generate_primdata()'s internal fill branch and fail NC_FILLVALUE check.
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol t_int = MakePrimTypeSymbol("int", NC_INT);
    return CallGenerateBasetypeExpectFinalize(t_int.get(), nullptr, codebuf,
                                              filldatalist, gen);
  }));

  // 15. NC_ECONST assigned to non-NC_ENUM type (`NC_INT`).
  EXPECT_TRUE(expect_semerror([](Generator* gen, Bytebuffer* codebuf) -> bool {
    UniqueSymbol t_int = MakePrimTypeSymbol("int", NC_INT);
    UniqueSymbol enum_t = MakeEnumTypeSymbol("en_t", t_int.get(), NC_INT);
    UniqueSymbol ec = MakeEnumConstSymbol("E0", enum_t.get(), 1);
    UniqueConstant ec_con(MakeConst(NC_ECONST, 20));
    ec_con->value.enumv = ec.get();
    return CallGenerateBasetypeExpectFinalize(t_int.get(), ec_con.get(),
                                              codebuf, nullptr, gen);
  }));
}

}  // namespace
}  // namespace netcdf
