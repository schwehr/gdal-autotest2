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

// Unit tests for //third_party/netcdf/ncgen/genchar.c.
//
// Exercises gen_chararray(), gen_charattr(), gen_charseq(), fill-char
// extraction, singleton char/byte merging, dimensional padding across rank-0,
// rank-1, and multi-dimensional fixed/unlimited Dimsets, warning paths, and
// fatal semerror() paths inside isolated forked children.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/genchar.c

#include <csetjmp>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
// clang-format off
#include "third_party/netcdf/google/util/ncgen_main_shim.h"
// clang-format on

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
    static char kCdlName[] = "genchar_test.cdl";
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

NCConstant* MakeCharConst(char value, int line = 1) {
  NCConstant* con = nullconst();
  con->nctype = NC_CHAR;
  con->lineno = line;
  con->value.charv = value;
  return con;
}

NCConstant* MakeByteConst(signed char value, int line = 1) {
  NCConstant* con = nullconst();
  con->nctype = NC_BYTE;
  con->lineno = line;
  con->value.int8v = value;
  return con;
}

NCConstant* MakeUbyteConst(unsigned char value, int line = 1) {
  NCConstant* con = nullconst();
  con->nctype = NC_UBYTE;
  con->lineno = line;
  con->value.uint8v = value;
  return con;
}

NCConstant* MakeIntConst(int value, int line = 1) {
  NCConstant* con = nullconst();
  con->nctype = NC_INT;
  con->lineno = line;
  con->value.int32v = value;
  return con;
}

NCConstant* MakeStringConst(std::string_view text, int line = 1) {
  NCConstant* con = nullconst();
  con->nctype = NC_STRING;
  con->lineno = line;
  con->value.stringv.len = text.size();
  auto* buf = static_cast<char*>(ecalloc(text.size() + 1));
  if (!text.empty()) {
    std::memcpy(buf, text.data(), text.size());
  }
  buf[text.size()] = '\0';
  con->value.stringv.stringv = buf;
  return con;
}

UniqueSymbol MakeDimSymbol(std::string_view name, size_t declsize,
                           bool is_unlimited) {
  auto* sym = static_cast<Symbol*>(ecalloc(sizeof(Symbol)));
  sym->objectclass = NC_DIM;
  sym->name = static_cast<char*>(ecalloc(name.size() + 1));
  std::memcpy(sym->name, name.data(), name.size());
  sym->dim.declsize = declsize;
  sym->dim.isunlimited = is_unlimited ? 1 : 0;
  return UniqueSymbol(sym);
}

std::string_view BufferBytes(const Bytebuffer* bb) {
  if (bb == nullptr || bbLength(bb) == 0) {
    return "";
  }
  return std::string_view(bbContents(bb), bbLength(bb));
}

bool ContainsConstPtr(const Datalist* dl, const NCConstant* target) {
  if (dl == nullptr) {
    return false;
  }
  for (size_t i = 0; i < datalistlen(dl); ++i) {
    if (datalistith(dl, i) == target) {
      return true;
    }
  }
  return false;
}

// Keep sigsetjmp in a plain C-compatible frame with no C++ destructors.
bool CallGenCharseqExpectFinalize(Datalist* data, Bytebuffer* databuf) {
  sigjmp_buf env;
  int retcode = -1;
  if (sigsetjmp(env, 1) != 0) {
    ncgen_shim_set_finalize_jmp(nullptr, nullptr);
    return retcode == 1;
  }
  ncgen_shim_set_finalize_jmp(&env, &retcode);
  gen_charseq(data, databuf);
  ncgen_shim_set_finalize_jmp(nullptr, nullptr);
  return false;
}

TEST(GencharTest, GenCharAttrAndSeqHandleStringableConstants) {
  ScopedNcgenGlobals globals;

  UniqueDatalist data(builddatalist(8));
  dlappend(data.get(), MakeCharConst('A'));
  dlappend(data.get(), MakeByteConst(static_cast<signed char>('B')));
  dlappend(data.get(), MakeUbyteConst(static_cast<unsigned char>('C')));
  dlappend(data.get(), MakeStringConst("de"));
  dlappend(data.get(), emptystringconst(2));
  dlappend(data.get(), MakeStringConst("f"));

  UniqueBytebuffer attr_buf(bbNew());
  gen_charattr(data.get(), attr_buf.get());
  EXPECT_EQ("ABCdef", BufferBytes(attr_buf.get()));

  UniqueBytebuffer seq_buf(bbNew());
  gen_charseq(data.get(), seq_buf.get());
  EXPECT_EQ("ABCdef", BufferBytes(seq_buf.get()));

  UniqueDatalist empty_dl(builddatalist(1));
  UniqueBytebuffer empty_buf(bbNew());
  gen_charattr(empty_dl.get(), empty_buf.get());
  EXPECT_EQ(0u, bbLength(empty_buf.get()));
}

TEST(GencharTest, GenCharSeqTriggersSemerrorOnNonStringableConstant) {
  EXPECT_TRUE(RunInForkedChild([]() -> bool {
    ScopedNcgenGlobals globals;
    UniqueDatalist bad_dl(builddatalist(2));
    dlappend(bad_dl.get(), MakeStringConst("ok"));
    dlappend(bad_dl.get(), MakeIntConst(77, 12));
    UniqueBytebuffer out(bbNew());
    return CallGenCharseqExpectFinalize(bad_dl.get(), out.get());
  }));
}

TEST(GencharTest, GenCharArrayHandlesRank0AndRank1AndFillSources) {
  ScopedNcgenGlobals globals;

  // Rank 0: empty flattened datalist with expectedsize == 1 stays length 0.
  {
    Dimset dim0{};
    dim0.ndims = 0;
    UniqueDatalist empty_data(builddatalist(1));
    UniqueBytebuffer out(bbNew());
    gen_chararray(&dim0, 0, empty_data.get(), out.get(), nullptr);
    EXPECT_EQ(0u, bbLength(out.get()));
  }

  // Rank 1 fixed size 6 with string fillsrc '*' and partial data "hi".
  {
    UniqueSymbol d0 = MakeDimSymbol("len6", 6, /*is_unlimited=*/false);
    Dimset dim1{};
    dim1.ndims = 1;
    dim1.dimsyms[0] = d0.get();

    UniqueDatalist fillsrc(builddatalist(1));
    dlappend(fillsrc.get(), MakeStringConst("*"));

    UniqueDatalist data(builddatalist(2));
    dlappend(data.get(), MakeStringConst("hi"));

    UniqueBytebuffer out(bbNew());
    gen_chararray(&dim1, 0, data.get(), out.get(), fillsrc.get());
    EXPECT_EQ("hi****", BufferBytes(out.get()));
  }

  // Rank 1 unlimited size 4 with NC_CHAR fillsrc and empty fill string
  // fallback.
  {
    UniqueSymbol u0 = MakeDimSymbol("unlim4", 4, /*is_unlimited=*/true);
    Dimset dim1{};
    dim1.ndims = 1;
    dim1.dimsyms[0] = u0.get();

    UniqueDatalist char_fill(builddatalist(1));
    dlappend(char_fill.get(), MakeCharConst('?'));

    UniqueDatalist data1(builddatalist(2));
    dlappend(data1.get(), MakeCharConst('Z'));
    UniqueBytebuffer out1(bbNew());
    gen_chararray(&dim1, 0, data1.get(), out1.get(), char_fill.get());
    EXPECT_EQ("Z???", BufferBytes(out1.get()));

    // Empty string fillsrc + empty fillsrc list both fall back to NC_FILL_CHAR.
    UniqueDatalist empty_str_fill(builddatalist(1));
    dlappend(empty_str_fill.get(), emptystringconst(1));
    UniqueDatalist data2(builddatalist(1));
    dlappend(data2.get(), MakeStringConst("ab"));
    UniqueBytebuffer out2(bbNew());
    gen_chararray(&dim1, 0, data2.get(), out2.get(), empty_str_fill.get());
    const std::string expected2 =
        std::string("ab") + std::string(2, NC_FILL_CHAR);
    EXPECT_EQ(expected2, BufferBytes(out2.get()));

    UniqueDatalist empty_fill(builddatalist(1));
    UniqueDatalist data3(builddatalist(1));
    dlappend(data3.get(), MakeStringConst("w"));
    UniqueBytebuffer out3(bbNew());
    gen_chararray(&dim1, 0, data3.get(), out3.get(), empty_fill.get());
    const std::string expected3 =
        std::string("w") + std::string(3, NC_FILL_CHAR);
    EXPECT_EQ(expected3, BufferBytes(out3.get()));
  }
}

TEST(GencharTest, GenCharArrayHandles2DFixedNestedPaddingAndNonStringWarn) {
  ScopedNcgenGlobals globals;

  UniqueSymbol rdim = MakeDimSymbol("rows", 3, /*is_unlimited=*/false);
  UniqueSymbol cdim = MakeDimSymbol("cols", 4, /*is_unlimited=*/false);
  Dimset dim2{};
  dim2.ndims = 2;
  dim2.dimsyms[0] = rdim.get();
  dim2.dimsyms[1] = cdim.get();

  UniqueDatalist fillsrc(builddatalist(1));
  dlappend(fillsrc.get(), MakeCharConst('_'));

  // Row 0: exact length 4 ("abcd") -> no constant padding.
  // Row 1: short length 2 ("xy") -> padded to "xy__".
  // Row 2: empty string "" -> padded to "____".
  // Total = 12 == expectedsize -> no trailing buffer padding.
  Datalist* row0 = builddatalist(1);
  dlappend(row0, MakeStringConst("abcd"));
  Datalist* row1 = builddatalist(1);
  dlappend(row1, MakeStringConst("xy"));
  Datalist* row2 = builddatalist(1);
  dlappend(row2, emptystringconst(3));

  UniqueDatalist data(builddatalist(3));
  dlappend(data.get(), builddatasublist(row0));
  dlappend(data.get(), builddatasublist(row1));
  dlappend(data.get(), builddatasublist(row2));

  UniqueBytebuffer out(bbNew());
  gen_chararray(&dim2, 0, data.get(), out.get(), fillsrc.get());
  EXPECT_EQ("abcdxy______", BufferBytes(out.get()));

  // Non-stringable NC_INT in non-singleton-merged list triggers semwarn and
  // pads remainder to expectedsize; NC_INT fillsrc falls back to NC_FILL_CHAR.
  UniqueDatalist int_fill(builddatalist(1));
  dlappend(int_fill.get(), MakeIntConst(0));

  UniqueDatalist warn_data(builddatalist(3));
  dlappend(warn_data.get(), MakeUbyteConst(static_cast<unsigned char>('K')));
  dlappend(warn_data.get(), MakeIntConst(99, 7));

  UniqueBytebuffer warn_out(bbNew());
  gen_chararray(&dim2, 0, warn_data.get(), warn_out.get(), int_fill.get());
  const std::string expected_warn =
      std::string("K") + std::string(11, NC_FILL_CHAR);
  EXPECT_EQ(expected_warn, BufferBytes(warn_out.get()));
}

TEST(GencharTest, GenCharArrayHandlesTrailingUnlimitedDim) {
  ScopedNcgenGlobals globals;

  // (fixed(2), unlimited(3)) => nc3unlim == 0 and isunlimited(rank - 1) == 1.
  UniqueSymbol d0 = MakeDimSymbol("fixed2", 2, /*is_unlimited=*/false);
  UniqueSymbol d1 = MakeDimSymbol("unlim3", 3, /*is_unlimited=*/true);
  Dimset dim2{};
  dim2.ndims = 2;
  dim2.dimsyms[0] = d0.get();
  dim2.dimsyms[1] = d1.get();

  UniqueDatalist fillsrc(builddatalist(1));
  dlappend(fillsrc.get(), MakeCharConst('.'));

  UniqueDatalist data(builddatalist(2));
  dlappend(data.get(), MakeStringConst("hi"));

  UniqueBytebuffer out(bbNew());
  gen_chararray(&dim2, 0, data.get(), out.get(), fillsrc.get());
  EXPECT_EQ("hi....", BufferBytes(out.get()));
}

TEST(GencharTest, RebuildSingletonsMergesAdjacentCharsAndHandlesSeparators) {
  ScopedNcgenGlobals globals;

  UniqueSymbol d0 = MakeDimSymbol("len12", 12, /*is_unlimited=*/false);
  Dimset dim1{};
  dim1.ndims = 1;
  dim1.dimsyms[0] = d0.get();

  // Leading non-char/non-byte constant keeps len == 0 -> preserved in newlist.
  // Next 'p', NC_BYTE('\0'), 'q' trigger accumulation (with embedded NUL!).
  // Next NC_UBYTE ends accumulation (len > 0); note upstream
  // rebuildsingletons() drops that separator constant without freeing it, so
  // we track and reclaim it cleanly if dropped.
  // Trailing 'r', NC_BYTE('s') hits unclosed-string finalization.
  NCConstant* sep = MakeUbyteConst(static_cast<unsigned char>('X'));
  auto reclaim_sep_if_dropped = absl::MakeCleanup([&]() {
    if (sep != nullptr) {
      reclaimconstant(sep);
    }
  });

  UniqueDatalist data(builddatalist(8));
  dlappend(data.get(), MakeStringConst("hd"));
  dlappend(data.get(), MakeCharConst('p'));
  dlappend(data.get(), MakeByteConst('\0'));
  dlappend(data.get(), MakeCharConst('q'));
  dlappend(data.get(), sep);
  dlappend(data.get(), MakeCharConst('r'));
  dlappend(data.get(), MakeByteConst(static_cast<signed char>('s')));

  UniqueDatalist fillsrc(builddatalist(1));
  dlappend(fillsrc.get(), MakeCharConst('-'));

  UniqueBytebuffer out(bbNew());
  gen_chararray(&dim1, 0, data.get(), out.get(), fillsrc.get());
  if (ContainsConstPtr(data.get(), sep)) {
    sep = nullptr;  // Owned by `data` and freed by UniqueDatalist.
  }

  const std::string expected = std::string("hdp\0qrs-----", 12);
  EXPECT_EQ(expected, BufferBytes(out.get()));
}

}  // namespace
}  // namespace netcdf
