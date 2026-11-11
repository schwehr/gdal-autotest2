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

// Unit tests for //third_party/netcdf/ncgen/cdata.c.
//
// Exercises all C language generator callbacks (charconstant, constant across
// all supported netCDF constant kinds, list framing across ListClass values,
// vlendecl, and vlenstring).
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/cdata.c

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"
// clang-format on

namespace netcdf {
namespace {

struct BbDeleter {
  void operator()(Bytebuffer* bb) const { bbFree(bb); }
};
using ScopedBb = std::unique_ptr<Bytebuffer, BbDeleter>;

// Zeroes all bytes of NCConstant (including all union members of Constvalue,
// which C++ aggregate `{}` value-initialization leaves uninitialized past the
// first union member `compoundv`).
NCConstant MakeZeroedConstant(nc_type nctype) {
  NCConstant con;
  std::memset(&con, 0, sizeof(con));
  con.nctype = nctype;
  return con;
}

void CleanupNcgenGlobals() {
  if (c_generator != nullptr) {
    generator_reset(c_generator, nullptr);
  }
  if (filldatalist != nullptr) {
    filldatalist->readonly = 0;
    freedatalist(filldatalist);
    filldatalist = nullptr;
  }
  if (codebuffer != nullptr) {
    bbFree(codebuffer);
    codebuffer = nullptr;
  }
  if (stmt != nullptr) {
    bbFree(stmt);
    stmt = nullptr;
  }
}

class CdataTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CleanupNcgenGlobals();
    ncgen_shim_reset_globals();
  }

  void TearDown() override { CleanupNcgenGlobals(); }
};

TEST_F(CdataTest, CharconstantTransfersEmptyAndPopulatedBuffers) {
  ScopedBb out(bbNew());
  ScopedBb empty_chars(bbNew());
  ASSERT_NE(out.get(), nullptr);
  ASSERT_NE(empty_chars.get(), nullptr);

  EXPECT_EQ(1, c_generator->charconstant(c_generator, nullptr, out.get(),
                                         empty_chars.get()));
  EXPECT_STREQ("", bbContents(out.get()));

  ScopedBb chars(bbNew());
  ASSERT_NE(chars.get(), nullptr);
  ASSERT_EQ(1, bbCat(chars.get(), "xyz"));
  EXPECT_EQ(1, c_generator->charconstant(c_generator, nullptr, out.get(),
                                         chars.get()));
  EXPECT_STREQ("xyz", bbContents(out.get()));
}

TEST_F(CdataTest, ConstantFormatsNumericCharAndFloatSpecialCases) {
  ScopedBb buf(bbNew());
  ASSERT_NE(buf.get(), nullptr);

  auto format_const = [&buf](const NCConstant& con) -> const char* {
    bbClear(buf.get());
    NCConstant copy = con;
    EXPECT_EQ(1, c_generator->constant(c_generator, nullptr, &copy, buf.get()));
    bbNull(buf.get());
    return bbContents(buf.get());
  };

  {
    NCConstant con = MakeZeroedConstant(NC_CHAR);
    con.value.charv = '\'';
    EXPECT_STREQ("'\\''", format_const(con));

    con.value.charv = 'Z';
    EXPECT_STREQ("'Z'", format_const(con));

    con.value.charv = '\n';
    EXPECT_STREQ("'\\n'", format_const(con));
  }
  {
    NCConstant con = MakeZeroedConstant(NC_BYTE);
    con.value.int8v = -12;
    EXPECT_STREQ("-12", format_const(con));
  }
  {
    NCConstant con = MakeZeroedConstant(NC_SHORT);
    con.value.int16v = -1234;
    EXPECT_STREQ("-1234", format_const(con));
  }
  {
    NCConstant con = MakeZeroedConstant(NC_INT);
    con.value.int32v = -123456;
    EXPECT_STREQ("-123456", format_const(con));
  }
  {
    NCConstant con = MakeZeroedConstant(NC_FLOAT);
    con.value.floatv = 1.5f;
    EXPECT_STREQ("((float)1.5)", format_const(con));

    con.value.floatv = std::numeric_limits<float>::quiet_NaN();
    EXPECT_STREQ("nanf", format_const(con));
  }
  {
    NCConstant con = MakeZeroedConstant(NC_DOUBLE);
    con.value.doublev = 2.25;
    EXPECT_STREQ("((double)2.25)", format_const(con));

    con.value.doublev = std::numeric_limits<double>::quiet_NaN();
    EXPECT_STREQ("nan", format_const(con));
  }
  {
    NCConstant con = MakeZeroedConstant(NC_UBYTE);
    con.value.uint8v = 200;
    EXPECT_STREQ("200U", format_const(con));
  }
  {
    NCConstant con = MakeZeroedConstant(NC_USHORT);
    con.value.uint16v = 50000;
    EXPECT_STREQ("50000U", format_const(con));
  }
  {
    NCConstant con = MakeZeroedConstant(NC_UINT);
    con.value.uint32v = 3000000000U;
    EXPECT_STREQ("3000000000U", format_const(con));
  }
  {
    NCConstant con = MakeZeroedConstant(NC_INT64);
    con.value.int64v = -9000000000000LL;
    EXPECT_STREQ("-9000000000000LL", format_const(con));
  }
  {
    NCConstant con = MakeZeroedConstant(NC_UINT64);
    con.value.uint64v = 18000000000000ULL;
    EXPECT_STREQ("18000000000000ULL", format_const(con));
  }
}

TEST_F(CdataTest, ConstantFormatsEnumStringNilAndOpaque) {
  l_flag = L_C;

  ScopedBb buf(bbNew());
  ASSERT_NE(buf.get(), nullptr);

  // Enum constant in a non-root group uses fqn.
  char enum_name[] = "RED";
  char enum_fqn[] = "grp_color_RED";
  Symbol enum_sym{};
  enum_sym.objectclass = NC_TYPE;
  enum_sym.subclass = NC_ECONST;
  enum_sym.name = enum_name;
  enum_sym.fqn = enum_fqn;
  enum_sym.grp.is_root = 0;

  NCConstant enum_con = MakeZeroedConstant(NC_ECONST);
  enum_con.value.enumv = &enum_sym;
  EXPECT_EQ(1,
            c_generator->constant(c_generator, nullptr, &enum_con, buf.get()));
  bbNull(buf.get());
  EXPECT_STREQ("grp_color_RED", bbContents(buf.get()));

  // NC_NIL and empty-NULL NC_STRING emit NULL.
  bbClear(buf.get());
  NCConstant nil_con = MakeZeroedConstant(NC_NIL);
  nil_con.value.stringv.len = 0;
  nil_con.value.stringv.stringv = nullptr;
  EXPECT_EQ(1,
            c_generator->constant(c_generator, nullptr, &nil_con, buf.get()));
  bbNull(buf.get());
  EXPECT_STREQ("NULL", bbContents(buf.get()));

  bbClear(buf.get());
  NCConstant null_str_con = MakeZeroedConstant(NC_STRING);
  null_str_con.value.stringv.len = 0;
  null_str_con.value.stringv.stringv = nullptr;
  EXPECT_EQ(
      1, c_generator->constant(c_generator, nullptr, &null_str_con, buf.get()));
  bbNull(buf.get());
  EXPECT_STREQ("NULL", bbContents(buf.get()));

  bbClear(buf.get());
  char empty_str[] = "";
  NCConstant empty_str_con = MakeZeroedConstant(NC_STRING);
  empty_str_con.value.stringv.len = 0;
  empty_str_con.value.stringv.stringv = empty_str;
  EXPECT_EQ(1, c_generator->constant(c_generator, nullptr, &empty_str_con,
                                     buf.get()));
  bbNull(buf.get());
  EXPECT_STREQ("\"\"", bbContents(buf.get()));

  // Populated NC_STRING quotes and escapes special characters and NULs.
  bbClear(buf.get());
  char msg[] = {'a', '"', 'b', '\0', 'c'};
  NCConstant str_con = MakeZeroedConstant(NC_STRING);
  str_con.value.stringv.len = 5;
  str_con.value.stringv.stringv = msg;
  EXPECT_EQ(1,
            c_generator->constant(c_generator, nullptr, &str_con, buf.get()));
  bbNull(buf.get());
  EXPECT_STREQ("\"a\\\"b\\000c\"", bbContents(buf.get()));

  // Empty and populated NC_OPAQUE format pairs of hex digits as \xNN bytes.
  bbClear(buf.get());
  char empty_hex[] = "";
  NCConstant empty_opq = MakeZeroedConstant(NC_OPAQUE);
  empty_opq.value.opaquev.len = 0;
  empty_opq.value.opaquev.stringv = empty_hex;
  EXPECT_EQ(1,
            c_generator->constant(c_generator, nullptr, &empty_opq, buf.get()));
  bbNull(buf.get());
  EXPECT_STREQ("\"\"", bbContents(buf.get()));

  bbClear(buf.get());
  char opq_hex[] = "cafe";
  NCConstant opq_con = MakeZeroedConstant(NC_OPAQUE);
  opq_con.value.opaquev.len = 4;
  opq_con.value.opaquev.stringv = opq_hex;
  EXPECT_EQ(1,
            c_generator->constant(c_generator, nullptr, &opq_con, buf.get()));
  bbNull(buf.get());
  EXPECT_STREQ("\"\\xcafe\\xfe\"", bbContents(buf.get()));
}

TEST_F(CdataTest, ListFramingHandlesAllListClassesAndCounts) {
  ScopedBb buf(bbNew());
  ASSERT_NE(buf.get(), nullptr);

  int uid1 = 0;
  int uid2 = 0;
  // LISTVLEN & LISTATTR do not wrap in braces; list() adds ", " when count>0.
  EXPECT_EQ(1, c_generator->listbegin(c_generator, nullptr, nullptr, LISTVLEN,
                                      2, buf.get(), &uid1));
  EXPECT_EQ(1, c_generator->list(c_generator, nullptr, nullptr, LISTVLEN, uid1,
                                 0, buf.get()));
  EXPECT_EQ(1, bbCat(buf.get(), "1"));
  EXPECT_EQ(1, c_generator->list(c_generator, nullptr, nullptr, LISTVLEN, uid1,
                                 1, buf.get()));
  EXPECT_EQ(1, bbCat(buf.get(), "2"));
  EXPECT_EQ(1, c_generator->listend(c_generator, nullptr, nullptr, LISTVLEN,
                                    uid1, 2, buf.get()));
  bbNull(buf.get());
  EXPECT_STREQ("1, 2", bbContents(buf.get()));

  bbClear(buf.get());
  EXPECT_EQ(1, c_generator->listbegin(c_generator, nullptr, nullptr, LISTATTR,
                                      2, buf.get(), nullptr));
  EXPECT_EQ(1, c_generator->list(c_generator, nullptr, nullptr, LISTATTR, 0, 0,
                                 buf.get()));
  EXPECT_EQ(1, bbCat(buf.get(), "a"));
  EXPECT_EQ(1, c_generator->list(c_generator, nullptr, nullptr, LISTATTR, 0, 1,
                                 buf.get()));
  EXPECT_EQ(1, bbCat(buf.get(), "b"));
  EXPECT_EQ(1, c_generator->listend(c_generator, nullptr, nullptr, LISTATTR, 0,
                                    2, buf.get()));
  bbNull(buf.get());
  EXPECT_STREQ("a, b", bbContents(buf.get()));

  // LISTDATA inserts spaces without outer braces.
  bbClear(buf.get());
  EXPECT_EQ(1, c_generator->listbegin(c_generator, nullptr, nullptr, LISTDATA,
                                      2, buf.get(), &uid2));
  EXPECT_GT(uid2, uid1);
  EXPECT_EQ(1, bbCat(buf.get(), "3"));
  EXPECT_EQ(1, c_generator->list(c_generator, nullptr, nullptr, LISTDATA, uid2,
                                 1, buf.get()));
  EXPECT_EQ(1, bbCat(buf.get(), "4"));
  EXPECT_EQ(1, c_generator->listend(c_generator, nullptr, nullptr, LISTDATA,
                                    uid2, 2, buf.get()));
  bbNull(buf.get());
  EXPECT_STREQ("3 4", bbContents(buf.get()));

  // LISTCOMPOUND & LISTFIELDARRAY wrap with { ... } and separate via spaces.
  for (const ListClass lc : {LISTCOMPOUND, LISTFIELDARRAY}) {
    bbClear(buf.get());
    EXPECT_EQ(1, c_generator->listbegin(c_generator, nullptr, nullptr, lc, 2,
                                        buf.get(), nullptr));
    EXPECT_EQ(1, bbCat(buf.get(), "x"));
    EXPECT_EQ(1, c_generator->list(c_generator, nullptr, nullptr, lc, 0, 1,
                                   buf.get()));
    EXPECT_EQ(1, bbCat(buf.get(), "y"));
    EXPECT_EQ(1, c_generator->listend(c_generator, nullptr, nullptr, lc, 0, 2,
                                      buf.get()));
    bbNull(buf.get());
    EXPECT_STREQ("{x y}", bbContents(buf.get()));
  }
}

TEST_F(CdataTest, VlendeclAndVlenstringEmitDeclsAndSizes) {
  l_flag = L_C;

  List* declstack = listnew();
  ASSERT_NE(declstack, nullptr);
  const auto free_declstack = absl::MakeCleanup([&declstack] {
    generator_reset(c_generator, nullptr);
    while (listlength(declstack) > 0) {
      auto* decl = static_cast<Bytebuffer*>(listpop(declstack));
      bbFree(decl);
    }
    listfree(declstack);
  });
  generator_reset(c_generator, declstack);

  Symbol int_sym{};
  int_sym.objectclass = NC_TYPE;
  int_sym.subclass = NC_PRIM;
  int_sym.typ.typecode = NC_INT;

  Symbol vlen_sym{};
  vlen_sym.objectclass = NC_TYPE;
  vlen_sym.subclass = NC_VLEN;
  vlen_sym.typ.basetype = &int_sym;

  ScopedBb codebuf(bbNew());
  ScopedBb vlenbuf(bbNew());
  ASSERT_NE(codebuf.get(), nullptr);
  ASSERT_NE(vlenbuf.get(), nullptr);
  ASSERT_EQ(1, bbCat(vlenbuf.get(), "10 20 30"));

  EXPECT_EQ(1, c_generator->vlendecl(c_generator, &vlen_sym, codebuf.get(), 7,
                                     size_t{3}, vlenbuf.get()));
  bbNull(codebuf.get());
  EXPECT_STREQ("{3,(void*)vlen_7}", bbContents(codebuf.get()));
  ASSERT_EQ(1UL, listlength(declstack));
  auto* decl0 = static_cast<Bytebuffer*>(listget(declstack, 0));
  ASSERT_NE(decl0, nullptr);
  bbNull(decl0);
  EXPECT_STREQ("static const int vlen_7[] = {10, 20, 30} ;", bbContents(decl0));

  // User-defined basetype uses cname() -> codify() path in ctypename().
  char opq_name[] = "my_opq";
  char opq_fqn[] = "grp_my_opq";
  Symbol opq_sym{};
  opq_sym.objectclass = NC_TYPE;
  opq_sym.subclass = NC_OPAQUE;
  opq_sym.name = opq_name;
  opq_sym.fqn = opq_fqn;
  opq_sym.grp.is_root = 0;

  Symbol vlen_opq_sym{};
  vlen_opq_sym.objectclass = NC_TYPE;
  vlen_opq_sym.subclass = NC_VLEN;
  vlen_opq_sym.typ.basetype = &opq_sym;

  bbClear(codebuf.get());
  ScopedBb empty_vlenbuf(bbNew());
  ASSERT_NE(empty_vlenbuf.get(), nullptr);
  EXPECT_EQ(1, c_generator->vlendecl(c_generator, &vlen_opq_sym, codebuf.get(),
                                     8, size_t{0}, empty_vlenbuf.get()));
  bbNull(codebuf.get());
  EXPECT_STREQ("{0,(void*)vlen_8}", bbContents(codebuf.get()));
  ASSERT_EQ(2UL, listlength(declstack));
  auto* decl1 = static_cast<Bytebuffer*>(listget(declstack, 1));
  ASSERT_NE(decl1, nullptr);
  bbNull(decl1);
  EXPECT_THAT(bbContents(decl1),
              ::testing::HasSubstr("static const grp_my_opq vlen_8[] = {} ;"));

  // c_vlenstring sets uid and byte length when pointers are provided and is a
  // safe no-op when output pointers are null.
  ScopedBb vlen_str(bbNew());
  ASSERT_NE(vlen_str.get(), nullptr);
  ASSERT_EQ(1, bbCat(vlen_str.get(), "hello"));
  int str_uid1 = 0;
  size_t str_count = 0;
  EXPECT_EQ(1, c_generator->vlenstring(c_generator, nullptr, vlen_str.get(),
                                       &str_uid1, &str_count));
  EXPECT_GT(str_uid1, 0);
  EXPECT_EQ(5U, str_count);
  EXPECT_EQ(1, c_generator->vlenstring(c_generator, nullptr, vlen_str.get(),
                                       nullptr, nullptr));
}

}  // namespace
}  // namespace netcdf
