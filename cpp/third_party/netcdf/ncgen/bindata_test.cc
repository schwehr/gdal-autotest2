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

// Unit tests for //third_party/netcdf/ncgen/bindata.c.
//
// Exercises bin_generator callbacks (charconstant, aligned constant encoding,
// compound/data list framing, vlendecl, vlenstring) and binary_generate_data()
// across primitive, fill, enum, opaque, vlen, compound, and error paths.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/bindata.c

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"

namespace netcdf {
namespace {

struct BbDeleter {
  void operator()(Bytebuffer* bb) const { bbFree(bb); }
};
using ScopedBb = std::unique_ptr<Bytebuffer, BbDeleter>;

struct DatalistFreer {
  void operator()(Datalist* dl) const { freedatalist(dl); }
};
using ScopedDatalist = std::unique_ptr<Datalist, DatalistFreer>;

struct ListDeleter {
  void operator()(List* l) const { listfree(l); }
};
using ScopedList = std::unique_ptr<List, ListDeleter>;

template <typename T>
T ReadPod(const char* src) {
  T value{};
  std::memcpy(&value, src, sizeof(T));
  return value;
}

NCConstant MakeZeroConstant(nc_type nctype) {
  NCConstant con;
  std::memset(&con, 0, sizeof(con));
  con.nctype = nctype;
  return con;
}

Symbol MakeZeroSymbol() {
  Symbol sym;
  std::memset(&sym, 0, sizeof(sym));
  return sym;
}

Symbol MakePrimSym(nc_type type) {
  Symbol sym = MakeZeroSymbol();
  sym.objectclass = NC_TYPE;
  sym.subclass = NC_PRIM;
  sym.typ.typecode = type;
  sym.nc_id = type;
  return sym;
}

void CleanupNcgenGlobals() {
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

class BindataTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CleanupNcgenGlobals();
    ncgen_shim_reset_globals();
  }

  void TearDown() override { CleanupNcgenGlobals(); }
};

TEST_F(BindataTest, CharconstantTransfersEmptyAndPopulatedBuffers) {
  ScopedBb out(bbNew());
  ScopedBb empty_chars(bbNew());
  ASSERT_NE(out.get(), nullptr);
  ASSERT_NE(empty_chars.get(), nullptr);

  EXPECT_EQ(1, bin_generator->charconstant(bin_generator, nullptr, out.get(),
                                           empty_chars.get()));
  EXPECT_EQ(0U, bbLength(out.get()));

  ScopedBb chars(bbNew());
  ASSERT_NE(chars.get(), nullptr);
  ASSERT_EQ(1, bbCat(chars.get(), "abc"));
  EXPECT_EQ(1, bin_generator->charconstant(bin_generator, nullptr, out.get(),
                                           chars.get()));
  EXPECT_EQ(3U, bbLength(out.get()));
  EXPECT_STREQ("abc", bbContents(out.get()));
}

TEST_F(BindataTest, ConstantEncodesAndAlignsAllPrimitiveOpaqueAndStringTypes) {
  ScopedBb buf(bbNew());
  ASSERT_NE(buf.get(), nullptr);

  // Single-byte types without prior misalignment.
  {
    NCConstant con = MakeZeroConstant(NC_CHAR);
    con.value.charv = 'Z';
    EXPECT_EQ(1,
              bin_generator->constant(bin_generator, nullptr, &con, buf.get()));
    EXPECT_EQ(1U, bbLength(buf.get()));
    EXPECT_EQ('Z', bbContents(buf.get())[0]);
  }
  {
    bbClear(buf.get());
    NCConstant con = MakeZeroConstant(NC_BYTE);
    con.value.int8v = -12;
    EXPECT_EQ(1,
              bin_generator->constant(bin_generator, nullptr, &con, buf.get()));
    EXPECT_EQ(-12, static_cast<signed char>(bbContents(buf.get())[0]));
  }
  {
    bbClear(buf.get());
    NCConstant con = MakeZeroConstant(NC_UBYTE);
    con.value.uint8v = 200;
    EXPECT_EQ(1,
              bin_generator->constant(bin_generator, nullptr, &con, buf.get()));
    EXPECT_EQ(static_cast<unsigned char>(200),
              static_cast<unsigned char>(bbContents(buf.get())[0]));
  }

  // Opaque hex encoding.
  {
    bbClear(buf.get());
    char opq_hex[] = "deadbeef";
    NCConstant con = MakeZeroConstant(NC_OPAQUE);
    con.value.opaquev.len = 8;
    con.value.opaquev.stringv = opq_hex;
    EXPECT_EQ(1,
              bin_generator->constant(bin_generator, nullptr, &con, buf.get()));
    ASSERT_EQ(4U, bbLength(buf.get()));
    const unsigned char* bytes =
        reinterpret_cast<const unsigned char*>(bbContents(buf.get()));
    EXPECT_EQ(0xDE, bytes[0]);
    EXPECT_EQ(0xAD, bytes[1]);
    EXPECT_EQ(0xBE, bytes[2]);
    EXPECT_EQ(0xEF, bytes[3]);
  }

  // Aligned multi-byte scalars starting from a 1-byte misaligned offset.
  auto check_aligned_scalar = [&buf](const NCConstant& con, size_t align,
                                     size_t value_size, auto expected) {
    using ValueT = decltype(expected);
    bbClear(buf.get());
    ASSERT_EQ(1, bbAppend(buf.get(), '\x7f'));
    NCConstant copy;
    std::memcpy(&copy, &con, sizeof(copy));
    EXPECT_EQ(
        1, bin_generator->constant(bin_generator, nullptr, &copy, buf.get()));
    ASSERT_EQ(align + value_size, bbLength(buf.get()));
    for (size_t i = 1; i < align; ++i) {
      EXPECT_EQ('\0', bbContents(buf.get())[i]);
    }
    EXPECT_EQ(expected, ReadPod<ValueT>(bbContents(buf.get()) + align));
  };

  {
    NCConstant con = MakeZeroConstant(NC_SHORT);
    con.value.int16v = -1234;
    check_aligned_scalar(con, alignof(short), sizeof(short), short{-1234});
  }
  {
    NCConstant con = MakeZeroConstant(NC_USHORT);
    con.value.uint16v = 50000;
    check_aligned_scalar(con, alignof(unsigned short), sizeof(unsigned short),
                         static_cast<unsigned short>(50000));
  }
  {
    NCConstant con = MakeZeroConstant(NC_INT);
    con.value.int32v = -123456;
    check_aligned_scalar(con, alignof(int), sizeof(int), -123456);
  }
  {
    NCConstant con = MakeZeroConstant(NC_UINT);
    con.value.uint32v = 3000000000U;
    check_aligned_scalar(con, alignof(unsigned int), sizeof(unsigned int),
                         3000000000U);
  }
  {
    NCConstant con = MakeZeroConstant(NC_FLOAT);
    con.value.floatv = 1.5f;
    check_aligned_scalar(con, alignof(float), sizeof(float), 1.5f);
  }
  {
    NCConstant con = MakeZeroConstant(NC_DOUBLE);
    con.value.doublev = 2.25;
    check_aligned_scalar(con, alignof(double), sizeof(double), 2.25);
  }
  {
    NCConstant con = MakeZeroConstant(NC_INT64);
    con.value.int64v = -9000000000000LL;
    check_aligned_scalar(con, alignof(long long), sizeof(long long),
                         -9000000000000LL);
  }
  {
    NCConstant con = MakeZeroConstant(NC_UINT64);
    con.value.uint64v = 18000000000000ULL;
    check_aligned_scalar(con, alignof(unsigned long long),
                         sizeof(unsigned long long), 18000000000000ULL);
  }

  // NC_NIL and empty-NULL NC_STRING encode NULL pointers. Full zeroing of
  // NCConstant is required because C++ aggregate initialization only zeros the
  // first union member (compoundv), leaving stringv.stringv uninitialized.
  for (const nc_type t : {NC_NIL, NC_STRING}) {
    bbClear(buf.get());
    NCConstant con = MakeZeroConstant(t);
    EXPECT_EQ(1,
              bin_generator->constant(bin_generator, nullptr, &con, buf.get()));
    ASSERT_EQ(sizeof(char*), bbLength(buf.get()));
    EXPECT_EQ(nullptr, ReadPod<char*>(bbContents(buf.get())));
  }

  // Empty non-NULL NC_STRING allocates an empty NUL-terminated copy.
  {
    bbClear(buf.get());
    char empty_str[] = "";
    NCConstant con = MakeZeroConstant(NC_STRING);
    con.value.stringv.len = 0;
    con.value.stringv.stringv = empty_str;
    EXPECT_EQ(1,
              bin_generator->constant(bin_generator, nullptr, &con, buf.get()));
    ASSERT_EQ(sizeof(char*), bbLength(buf.get()));
    std::unique_ptr<char, decltype(&std::free)> emitted(
        ReadPod<char*>(bbContents(buf.get())), &std::free);
    ASSERT_NE(emitted.get(), nullptr);
    EXPECT_STREQ("", emitted.get());
  }

  // Populated NC_STRING aligns and writes a heap-allocated copy of the text.
  {
    bbClear(buf.get());
    ASSERT_EQ(1, bbAppend(buf.get(), '\x7f'));
    char str_data[] = "hello";
    NCConstant con = MakeZeroConstant(NC_STRING);
    con.value.stringv.len = 5;
    con.value.stringv.stringv = str_data;
    EXPECT_EQ(1,
              bin_generator->constant(bin_generator, nullptr, &con, buf.get()));
    ASSERT_EQ(alignof(char*) + sizeof(char*), bbLength(buf.get()));
    std::unique_ptr<char, decltype(&std::free)> emitted(
        ReadPod<char*>(bbContents(buf.get()) + alignof(char*)), &std::free);
    ASSERT_NE(emitted.get(), nullptr);
    EXPECT_STREQ("hello", emitted.get());
  }
}

TEST_F(BindataTest, ListFramingVlendeclAndVlenstringAlignAndSerialize) {
  ScopedBb buf(bbNew());
  ASSERT_NE(buf.get(), nullptr);

  // Non-compound list callbacks accept NULL liststate and NULL uidp.
  int uid1 = 0;
  EXPECT_EQ(1, bin_generator->listbegin(bin_generator, nullptr, nullptr,
                                        LISTDATA, 1, buf.get(), &uid1));
  EXPECT_GT(uid1, 0);
  EXPECT_EQ(1, bin_generator->list(bin_generator, nullptr, nullptr, LISTDATA,
                                   uid1, 0, buf.get()));
  EXPECT_EQ(1, bin_generator->listend(bin_generator, nullptr, nullptr, LISTDATA,
                                      uid1, 1, buf.get()));
  EXPECT_EQ(0U, bbLength(buf.get()));

  // LISTCOMPOUND aligns relative to offsetbase recorded in listbegin.
  ASSERT_EQ(1, bbAppend(buf.get(), '\x01'));  // 1-byte outer prefix
  size_t list_state = 0;
  Symbol field_sym = MakeZeroSymbol();
  field_sym.typ.alignment = 4;
  Symbol cmpd_sym = MakeZeroSymbol();
  cmpd_sym.typ.cmpdalign = 8;

  EXPECT_EQ(1, bin_generator->listbegin(bin_generator, &cmpd_sym, &list_state,
                                        LISTCOMPOUND, 1, buf.get(), nullptr));
  EXPECT_EQ(1U, list_state);

  ASSERT_EQ(1, bbAppend(buf.get(), '\x02'));  // offset wrt base = 1
  EXPECT_EQ(1, bin_generator->list(bin_generator, &field_sym, &list_state,
                                   LISTCOMPOUND, 0, 0, buf.get()));
  // Aligned to 4 wrt base=1 -> length 1 + 4 = 5 (3 pad bytes).
  EXPECT_EQ(5U, bbLength(buf.get()));

  // Already aligned (offset wrt base = 4) -> no extra padding added.
  EXPECT_EQ(1, bin_generator->list(bin_generator, &field_sym, &list_state,
                                   LISTCOMPOUND, 0, 1, buf.get()));
  EXPECT_EQ(5U, bbLength(buf.get()));

  ASSERT_EQ(1, bbAppend(buf.get(), '\x03'));  // offset wrt base = 5
  EXPECT_EQ(1, bin_generator->listend(bin_generator, &cmpd_sym, &list_state,
                                      LISTCOMPOUND, 0, 1, buf.get()));
  // Aligned to cmpdalign=8 wrt base=1 -> length 1 + 8 = 9 (3 pad bytes).
  EXPECT_EQ(9U, bbLength(buf.get()));
  EXPECT_EQ(1, bin_generator->listend(bin_generator, &cmpd_sym, &list_state,
                                      LISTCOMPOUND, 0, 1, buf.get()));
  EXPECT_EQ(9U, bbLength(buf.get()));

  // bin_vlendecl extracts buffer content into nc_vlen_t.
  bbClear(buf.get());
  ScopedBb vlen_mem(bbNew());
  ASSERT_NE(vlen_mem.get(), nullptr);
  ASSERT_EQ(1, bbCat(vlen_mem.get(), "ABCD"));
  EXPECT_EQ(1, bin_generator->vlendecl(bin_generator, nullptr, buf.get(), 1,
                                       size_t{4}, vlen_mem.get()));
  EXPECT_EQ(0U, bbLength(vlen_mem.get()));
  ASSERT_EQ(sizeof(nc_vlen_t), bbLength(buf.get()));
  nc_vlen_t decl_vlen = ReadPod<nc_vlen_t>(bbContents(buf.get()));
  std::unique_ptr<void, decltype(&std::free)> decl_p(decl_vlen.p, &std::free);
  EXPECT_EQ(4U, decl_vlen.len);
  ASSERT_NE(decl_p.get(), nullptr);
  EXPECT_EQ(0, std::memcmp(decl_p.get(), "ABCD", 4));

  // bin_vlenstring duplicates vlenmem with uidp non-null and null.
  bbClear(buf.get());
  ScopedBb str_vlen_mem(bbNew());
  ASSERT_NE(str_vlen_mem.get(), nullptr);
  ASSERT_EQ(1, bbCat(str_vlen_mem.get(), "hello"));
  int uid2 = 0;
  size_t unused_size = 0;
  EXPECT_EQ(1,
            bin_generator->vlenstring(bin_generator, nullptr, buf.get(), &uid2,
                                      &unused_size, str_vlen_mem.get()));
  EXPECT_GT(uid2, uid1);
  ASSERT_EQ(sizeof(nc_vlen_t), bbLength(buf.get()));
  nc_vlen_t str_vlen1 = ReadPod<nc_vlen_t>(bbContents(buf.get()));
  std::unique_ptr<void, decltype(&std::free)> str_p1(str_vlen1.p, &std::free);
  EXPECT_EQ(5U, str_vlen1.len);
  ASSERT_NE(str_p1.get(), nullptr);
  EXPECT_STREQ("hello", static_cast<const char*>(str_p1.get()));

  bbClear(buf.get());
  ScopedBb empty_vlen_mem(bbNew());
  ASSERT_NE(empty_vlen_mem.get(), nullptr);
  ASSERT_EQ(1, bbNull(empty_vlen_mem.get()));
  EXPECT_EQ(
      1, bin_generator->vlenstring(bin_generator, nullptr, buf.get(), nullptr,
                                   nullptr, empty_vlen_mem.get()));
  nc_vlen_t str_vlen2 = ReadPod<nc_vlen_t>(bbContents(buf.get()));
  std::unique_ptr<void, decltype(&std::free)> str_p2(str_vlen2.p, &std::free);
  EXPECT_EQ(0U, str_vlen2.len);
  EXPECT_STREQ("", static_cast<const char*>(str_p2.get()));
}

TEST_F(BindataTest, BinaryGenerateDataCoversAllPrimitivesFillValuesAndErrors) {
  ScopedBb databuf(bbNew());
  ASSERT_NE(databuf.get(), nullptr);

  // Empty Datalist clears databuf and returns NC_NOERR.
  ASSERT_EQ(1, bbCat(databuf.get(), "stale"));
  Symbol int_sym = MakePrimSym(NC_INT);
  ScopedDatalist empty_dl(builddatalist(0));
  ASSERT_NE(empty_dl.get(), nullptr);
  EXPECT_EQ(NC_NOERR, binary_generate_data(empty_dl.get(), &int_sym, nullptr,
                                           databuf.get()));
  EXPECT_EQ(0U, bbLength(databuf.get()));

  auto generate_one_prim = [&databuf](nc_type prim_type, NCConstant* con,
                                      Symbol* sym_out) -> int {
    *sym_out = MakePrimSym(prim_type);
    ScopedDatalist dl(builddatalist(1));
    dlappend(dl.get(), con);
    return binary_generate_data(dl.get(), sym_out, nullptr, databuf.get());
  };

  {
    Symbol sym = MakeZeroSymbol();
    NCConstant con = MakeZeroConstant(NC_CHAR);
    con.value.charv = 'K';
    EXPECT_EQ(NC_NOERR, generate_one_prim(NC_CHAR, &con, &sym));
    ASSERT_EQ(1U, bbLength(databuf.get()));
    EXPECT_EQ('K', bbContents(databuf.get())[0]);
  }
  {
    Symbol sym = MakeZeroSymbol();
    NCConstant con = MakeZeroConstant(NC_INT);
    con.value.int32v = -17;
    EXPECT_EQ(NC_NOERR, generate_one_prim(NC_BYTE, &con, &sym));
    ASSERT_EQ(1U, bbLength(databuf.get()));
    EXPECT_EQ(-17, static_cast<signed char>(bbContents(databuf.get())[0]));
  }
  {
    Symbol sym = MakeZeroSymbol();
    NCConstant con = MakeZeroConstant(NC_UINT);
    con.value.uint32v = 210U;
    EXPECT_EQ(NC_NOERR, generate_one_prim(NC_UBYTE, &con, &sym));
    ASSERT_EQ(1U, bbLength(databuf.get()));
    EXPECT_EQ(static_cast<unsigned char>(210),
              static_cast<unsigned char>(bbContents(databuf.get())[0]));
  }
  {
    Symbol sym = MakeZeroSymbol();
    NCConstant con = MakeZeroConstant(NC_SHORT);
    con.value.int16v = -321;
    EXPECT_EQ(NC_NOERR, generate_one_prim(NC_SHORT, &con, &sym));
    ASSERT_EQ(sizeof(short), bbLength(databuf.get()));
    EXPECT_EQ(-321, ReadPod<short>(bbContents(databuf.get())));
  }
  {
    Symbol sym = MakeZeroSymbol();
    NCConstant con = MakeZeroConstant(NC_USHORT);
    con.value.uint16v = 43210;
    EXPECT_EQ(NC_NOERR, generate_one_prim(NC_USHORT, &con, &sym));
    ASSERT_EQ(sizeof(unsigned short), bbLength(databuf.get()));
    EXPECT_EQ(static_cast<unsigned short>(43210),
              ReadPod<unsigned short>(bbContents(databuf.get())));
  }
  {
    Symbol sym = MakeZeroSymbol();
    NCConstant con = MakeZeroConstant(NC_INT);
    con.value.int32v = -987654;
    EXPECT_EQ(NC_NOERR, generate_one_prim(NC_INT, &con, &sym));
    ASSERT_EQ(sizeof(int), bbLength(databuf.get()));
    EXPECT_EQ(-987654, ReadPod<int>(bbContents(databuf.get())));
  }
  {
    Symbol sym = MakeZeroSymbol();
    NCConstant con = MakeZeroConstant(NC_UINT);
    con.value.uint32v = 3210987654U;
    EXPECT_EQ(NC_NOERR, generate_one_prim(NC_UINT, &con, &sym));
    ASSERT_EQ(sizeof(unsigned int), bbLength(databuf.get()));
    EXPECT_EQ(3210987654U, ReadPod<unsigned int>(bbContents(databuf.get())));
  }
  {
    Symbol sym = MakeZeroSymbol();
    NCConstant con = MakeZeroConstant(NC_INT64);
    con.value.int64v = -9000000000000LL;
    EXPECT_EQ(NC_NOERR, generate_one_prim(NC_INT64, &con, &sym));
    ASSERT_EQ(sizeof(long long), bbLength(databuf.get()));
    EXPECT_EQ(-9000000000000LL, ReadPod<long long>(bbContents(databuf.get())));
  }
  {
    Symbol sym = MakeZeroSymbol();
    NCConstant con = MakeZeroConstant(NC_UINT64);
    con.value.uint64v = 18000000000000ULL;
    EXPECT_EQ(NC_NOERR, generate_one_prim(NC_UINT64, &con, &sym));
    ASSERT_EQ(sizeof(unsigned long long), bbLength(databuf.get()));
    EXPECT_EQ(18000000000000ULL,
              ReadPod<unsigned long long>(bbContents(databuf.get())));
  }
  {
    Symbol sym = MakeZeroSymbol();
    NCConstant con = MakeZeroConstant(NC_FLOAT);
    con.value.floatv = 3.5f;
    EXPECT_EQ(NC_NOERR, generate_one_prim(NC_FLOAT, &con, &sym));
    ASSERT_EQ(sizeof(float), bbLength(databuf.get()));
    EXPECT_FLOAT_EQ(3.5f, ReadPod<float>(bbContents(databuf.get())));
  }
  {
    Symbol sym = MakeZeroSymbol();
    NCConstant con = MakeZeroConstant(NC_DOUBLE);
    con.value.doublev = 6.25;
    EXPECT_EQ(NC_NOERR, generate_one_prim(NC_DOUBLE, &con, &sym));
    ASSERT_EQ(sizeof(double), bbLength(databuf.get()));
    EXPECT_DOUBLE_EQ(6.25, ReadPod<double>(bbContents(databuf.get())));
  }
  {
    Symbol sym = MakeZeroSymbol();
    char s[] = "bin_str";
    NCConstant con = MakeZeroConstant(NC_STRING);
    con.value.stringv.len = 7;
    con.value.stringv.stringv = s;
    ASSERT_EQ(NC_NOERR, generate_one_prim(NC_STRING, &con, &sym));
    ASSERT_EQ(sizeof(char*), bbLength(databuf.get()));
    std::unique_ptr<char, decltype(&std::free)> emitted(
        ReadPod<char*>(bbContents(databuf.get())), &std::free);
    ASSERT_NE(emitted.get(), nullptr);
    EXPECT_STREQ("bin_str", emitted.get());
  }

  // NC_FILLVALUE uses getfiller(tsym) when fillvalue is NULL, and uses
  // explicit fillvalue when provided.
  {
    Symbol sym = MakePrimSym(NC_INT);
    const auto clean_fill = absl::MakeCleanup([&sym] {
      reclaimdatalist(sym.typ._Fillvalue);
      sym.typ._Fillvalue = nullptr;
    });
    NCConstant fill_con = MakeZeroConstant(NC_FILLVALUE);
    ScopedDatalist dl(builddatalist(1));
    dlappend(dl.get(), &fill_con);
    ASSERT_EQ(NC_NOERR,
              binary_generate_data(dl.get(), &sym, nullptr, databuf.get()));
    ASSERT_EQ(sizeof(int), bbLength(databuf.get()));
    EXPECT_EQ(NC_FILL_INT, ReadPod<int>(bbContents(databuf.get())));

    NCConstant custom_fill = MakeZeroConstant(NC_INT);
    custom_fill.value.int32v = 999;
    ScopedDatalist explicit_fill(builddatalist(1));
    dlappend(explicit_fill.get(), &custom_fill);
    ASSERT_EQ(NC_NOERR,
              binary_generate_data(dl.get(), &sym, explicit_fill.get(),
                                   databuf.get()));
    ASSERT_EQ(sizeof(int), bbLength(databuf.get()));
    EXPECT_EQ(999, ReadPod<int>(bbContents(databuf.get())));
  }

  // Invalid primitive nc_id returns NC_EINTERNAL.
  {
    Symbol bad_prim = MakePrimSym(NC_NAT);
    NCConstant con = MakeZeroConstant(NC_INT);
    con.value.int32v = 1;
    ScopedDatalist dl(builddatalist(1));
    dlappend(dl.get(), &con);
    EXPECT_EQ(NC_EINTERNAL, binary_generate_data(dl.get(), &bad_prim, nullptr,
                                                 databuf.get()));
  }
}

TEST_F(BindataTest, BinaryGenerateDataCoversEnumOpaqueCompoundAndBadSubclass) {
  ScopedBb databuf(bbNew());
  ASSERT_NE(databuf.get(), nullptr);

  // NC_ENUM delegates to basetype; semantics sets typ.typecode to basetype's
  // typecode so default getfiller(tsym) emits basetype fill (NC_FILL_INT),
  // while explicit fillvalue can supply an NC_ECONST instance.
  {
    Symbol int_sym = MakePrimSym(NC_INT);
    NCConstant ec_val = MakeZeroConstant(NC_INT);
    ec_val.value.int32v = 77;
    Symbol econst_sym = MakeZeroSymbol();
    econst_sym.objectclass = NC_TYPE;
    econst_sym.subclass = NC_ECONST;
    econst_sym.typ.basetype = &int_sym;
    econst_sym.typ.econst = &ec_val;

    ScopedList enum_subnodes(listnew());
    ASSERT_NE(enum_subnodes.get(), nullptr);
    listpush(enum_subnodes.get(), &econst_sym);

    Symbol enum_sym = MakeZeroSymbol();
    enum_sym.objectclass = NC_TYPE;
    enum_sym.subclass = NC_ENUM;
    enum_sym.typ.typecode = NC_INT;
    enum_sym.typ.basetype = &int_sym;
    enum_sym.subnodes = enum_subnodes.get();
    const auto clean_enum_fill = absl::MakeCleanup([&enum_sym] {
      reclaimdatalist(enum_sym.typ._Fillvalue);
      enum_sym.typ._Fillvalue = nullptr;
    });

    NCConstant ec_inst = MakeZeroConstant(NC_ECONST);
    ec_inst.value.enumv = &econst_sym;
    NCConstant fill_inst = MakeZeroConstant(NC_FILLVALUE);

    ScopedDatalist dl(builddatalist(2));
    dlappend(dl.get(), &ec_inst);
    dlappend(dl.get(), &fill_inst);
    ASSERT_EQ(NC_NOERR, binary_generate_data(dl.get(), &enum_sym, nullptr,
                                             databuf.get()));
    ASSERT_EQ(2 * sizeof(int), bbLength(databuf.get()));
    EXPECT_EQ(77, ReadPod<int>(bbContents(databuf.get())));
    EXPECT_EQ(NC_FILL_INT,
              ReadPod<int>(bbContents(databuf.get()) + sizeof(int)));

    ScopedDatalist explicit_enum_fill(builddatalist(1));
    dlappend(explicit_enum_fill.get(), &ec_inst);
    ASSERT_EQ(NC_NOERR,
              binary_generate_data(dl.get(), &enum_sym,
                                   explicit_enum_fill.get(), databuf.get()));
    ASSERT_EQ(2 * sizeof(int), bbLength(databuf.get()));
    EXPECT_EQ(77, ReadPod<int>(bbContents(databuf.get())));
    EXPECT_EQ(77, ReadPod<int>(bbContents(databuf.get()) + sizeof(int)));
  }

  // NC_OPAQUE handles valid hex strings and rejects non-opaque constants.
  {
    Symbol opq_sym = MakeZeroSymbol();
    opq_sym.objectclass = NC_TYPE;
    opq_sym.subclass = NC_OPAQUE;
    opq_sym.typ.typecode = NC_OPAQUE;

    char opq_hex[] = "01ab";
    NCConstant valid_opq = MakeZeroConstant(NC_OPAQUE);
    valid_opq.value.opaquev.len = 4;
    valid_opq.value.opaquev.stringv = opq_hex;
    ScopedDatalist ok_dl(builddatalist(1));
    dlappend(ok_dl.get(), &valid_opq);
    ASSERT_EQ(NC_NOERR, binary_generate_data(ok_dl.get(), &opq_sym, nullptr,
                                             databuf.get()));
    ASSERT_EQ(2U, bbLength(databuf.get()));
    const unsigned char* bytes =
        reinterpret_cast<const unsigned char*>(bbContents(databuf.get()));
    EXPECT_EQ(0x01, bytes[0]);
    EXPECT_EQ(0xAB, bytes[1]);

    NCConstant bad_opq = MakeZeroConstant(NC_INT);
    bad_opq.value.int32v = 3;
    ScopedDatalist err_dl(builddatalist(1));
    dlappend(err_dl.get(), &bad_opq);
    EXPECT_EQ(NC_EBADTYPE, binary_generate_data(err_dl.get(), &opq_sym, nullptr,
                                                databuf.get()));
  }

  // Non-compound constant for NC_VLEN returns NC_EBADTYPE without allocating.
  {
    Symbol int_sym = MakePrimSym(NC_INT);
    Symbol vlen_sym = MakeZeroSymbol();
    vlen_sym.objectclass = NC_TYPE;
    vlen_sym.subclass = NC_VLEN;
    vlen_sym.typ.basetype = &int_sym;

    NCConstant bad_vlen = MakeZeroConstant(NC_INT);
    bad_vlen.value.int32v = 1;
    ScopedDatalist dl(builddatalist(1));
    dlappend(dl.get(), &bad_vlen);
    EXPECT_EQ(NC_EBADTYPE, binary_generate_data(dl.get(), &vlen_sym, nullptr,
                                                databuf.get()));
  }

  // NC_COMPOUND handles scalar and array fields plus field/compound alignment.
  {
    Symbol byte_sym = MakePrimSym(NC_BYTE);
    Symbol int_sym = MakePrimSym(NC_INT);

    Symbol dim1 = MakeZeroSymbol();
    dim1.dim.declsize = 1;
    Symbol dim2 = MakeZeroSymbol();
    dim2.dim.declsize = 2;

    // Note: bindata.c accesses dimsyms[0]->dim.declsize even when ndims == 0.
    Symbol f_byte = MakeZeroSymbol();
    f_byte.objectclass = NC_TYPE;
    f_byte.subclass = NC_FIELD;
    f_byte.typ.basetype = &byte_sym;
    f_byte.typ.alignment = 1;
    f_byte.typ.dimset.ndims = 0;
    f_byte.typ.dimset.dimsyms[0] = &dim1;

    Symbol f_int_arr = MakeZeroSymbol();
    f_int_arr.objectclass = NC_TYPE;
    f_int_arr.subclass = NC_FIELD;
    f_int_arr.typ.basetype = &int_sym;
    f_int_arr.typ.alignment = 4;
    f_int_arr.typ.dimset.ndims = 1;
    f_int_arr.typ.dimset.dimsyms[0] = &dim2;

    ScopedList subnodes(listnew());
    ASSERT_NE(subnodes.get(), nullptr);
    listpush(subnodes.get(), &f_byte);
    listpush(subnodes.get(), &f_int_arr);

    Symbol cmpd_sym = MakeZeroSymbol();
    cmpd_sym.objectclass = NC_TYPE;
    cmpd_sym.subclass = NC_COMPOUND;
    cmpd_sym.typ.typecode = NC_COMPOUND;
    cmpd_sym.typ.cmpdalign = 8;
    cmpd_sym.subnodes = subnodes.get();

    NCConstant b0 = MakeZeroConstant(NC_INT);
    b0.value.int32v = 7;
    NCConstant i0 = MakeZeroConstant(NC_INT);
    i0.value.int32v = 99;
    ScopedDatalist c0_fields(builddatalist(2));
    dlappend(c0_fields.get(), &b0);
    dlappend(c0_fields.get(), &i0);
    NCConstant inst0 = MakeZeroConstant(NC_COMPOUND);
    inst0.value.compoundv = c0_fields.get();

    NCConstant b1 = MakeZeroConstant(NC_INT);
    b1.value.int32v = 8;
    NCConstant i1 = MakeZeroConstant(NC_INT);
    i1.value.int32v = 55;
    ScopedDatalist c1_fields(builddatalist(2));
    dlappend(c1_fields.get(), &b1);
    dlappend(c1_fields.get(), &i1);
    NCConstant inst1 = MakeZeroConstant(NC_COMPOUND);
    inst1.value.compoundv = c1_fields.get();

    ScopedDatalist cmpd_dl(builddatalist(2));
    dlappend(cmpd_dl.get(), &inst0);
    dlappend(cmpd_dl.get(), &inst1);

    // Instance 0: [7] [0,0,0] [99, 99] (12 bytes)
    // Align cmpdalign=8 before instance 1 -> 4 zero bytes (offset 16)
    // Instance 1: [8] [0,0,0] [55, 55] (12 bytes -> total 28 bytes)
    ASSERT_EQ(NC_NOERR, binary_generate_data(cmpd_dl.get(), &cmpd_sym, nullptr,
                                             databuf.get()));
    ASSERT_EQ(28U, bbLength(databuf.get()));
    const char* raw = bbContents(databuf.get());
    EXPECT_EQ(7, static_cast<signed char>(raw[0]));
    EXPECT_EQ(99, ReadPod<int>(raw + 4));
    EXPECT_EQ(99, ReadPod<int>(raw + 8));
    EXPECT_EQ(8, static_cast<signed char>(raw[16]));
    EXPECT_EQ(55, ReadPod<int>(raw + 20));
    EXPECT_EQ(55, ReadPod<int>(raw + 24));

    // Propagate error from a compound field back through bin_generate_data_r.
    Symbol bad_opq_sym = MakeZeroSymbol();
    bad_opq_sym.objectclass = NC_TYPE;
    bad_opq_sym.subclass = NC_OPAQUE;
    f_byte.typ.basetype = &bad_opq_sym;
    EXPECT_EQ(NC_EBADTYPE, binary_generate_data(cmpd_dl.get(), &cmpd_sym,
                                                nullptr, databuf.get()));
  }

  // Unhandled symbol subclass returns NC_EINTERNAL.
  {
    Symbol bad_sym = MakeZeroSymbol();
    bad_sym.objectclass = NC_TYPE;
    bad_sym.subclass = NC_ARRAY;
    NCConstant con = MakeZeroConstant(NC_INT);
    con.value.int32v = 1;
    ScopedDatalist dl(builddatalist(1));
    dlappend(dl.get(), &con);
    EXPECT_EQ(NC_EINTERNAL,
              binary_generate_data(dl.get(), &bad_sym, nullptr, databuf.get()));
  }
}

TEST_F(BindataTest, BinaryGenerateDataHandlesVlenSuccessAndNestedErrorInChild) {
  // Valid NC_VLEN allocates a Bytebuffer struct in bin_generate_data_r that is
  // intentionally retained for nc_vlen_t::p; isolate in a forked child so LSan
  // remains clean while child gcov counters are flushed.
  ASSERT_TRUE(RunInForkedChild([]() -> bool {
    ScopedBb databuf(bbNew());
    if (databuf == nullptr) {
      return false;
    }

    Symbol int_sym = MakePrimSym(NC_INT);
    Symbol vlen_sym = MakeZeroSymbol();
    vlen_sym.objectclass = NC_TYPE;
    vlen_sym.subclass = NC_VLEN;
    vlen_sym.typ.basetype = &int_sym;

    NCConstant e0 = MakeZeroConstant(NC_INT);
    e0.value.int32v = 10;
    NCConstant e1 = MakeZeroConstant(NC_INT);
    e1.value.int32v = 20;
    ScopedDatalist sublist(builddatalist(2));
    dlappend(sublist.get(), &e0);
    dlappend(sublist.get(), &e1);

    NCConstant vlen_inst = MakeZeroConstant(NC_COMPOUND);
    vlen_inst.value.compoundv = sublist.get();

    ScopedDatalist dl(builddatalist(1));
    dlappend(dl.get(), &vlen_inst);

    if (binary_generate_data(dl.get(), &vlen_sym, nullptr, databuf.get()) !=
        NC_NOERR) {
      return false;
    }
    if (bbLength(databuf.get()) != sizeof(nc_vlen_t)) {
      return false;
    }
    const nc_vlen_t vl = ReadPod<nc_vlen_t>(bbContents(databuf.get()));
    if (vl.len != 2U || vl.p == nullptr) {
      return false;
    }
    const char* vals = static_cast<const char*>(vl.p);
    if (ReadPod<int>(vals) != 10 || ReadPod<int>(vals + sizeof(int)) != 20) {
      return false;
    }

    // Empty vlen sublist succeeds with len == 0.
    ScopedDatalist empty_sub(builddatalist(0));
    vlen_inst.value.compoundv = empty_sub.get();
    if (binary_generate_data(dl.get(), &vlen_sym, nullptr, databuf.get()) !=
        NC_NOERR) {
      return false;
    }
    const nc_vlen_t empty_vl = ReadPod<nc_vlen_t>(bbContents(databuf.get()));
    if (empty_vl.len != 0U) {
      return false;
    }

    // Nested basetype failure inside vlen propagates error status.
    Symbol bad_prim = MakePrimSym(NC_NAT);
    vlen_sym.typ.basetype = &bad_prim;
    vlen_inst.value.compoundv = sublist.get();
    return binary_generate_data(dl.get(), &vlen_sym, nullptr, databuf.get()) ==
           NC_EINTERNAL;
  }));
}

}  // namespace
}  // namespace netcdf
