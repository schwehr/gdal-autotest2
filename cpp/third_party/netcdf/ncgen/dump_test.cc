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

// Unit tests for //third_party/netcdf/ncgen/dump.c.
//
// Exercises datalist formatting via bufdump(), stderr printing via
// dumpdatalist(), dumpconstant(), dumpconstant1(), and dumpgroup() while
// sandboxing upstream dump routines with known internal allocations inside
// forked children for clean leak-sanitizer behavior.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/dump.c

#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"

extern "C" {
void dumpconstant1(NCConstant* con);
}  // extern "C"

namespace netcdf {
namespace {

template <typename Fn>
bool RunCapturingStderrInChild(Fn&& fn, std::string* captured_out) {
  int pipefd[2] = {-1, -1};
  if (::pipe(pipefd) != 0) {
    return false;
  }
  const int saved_stderr = ::dup(STDERR_FILENO);
  if (saved_stderr < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return false;
  }
  std::fflush(stderr);
  ::dup2(pipefd[1], STDERR_FILENO);
  ::close(pipefd[1]);

  std::forward<Fn>(fn)();
  std::fflush(stderr);

  ::dup2(saved_stderr, STDERR_FILENO);
  ::close(saved_stderr);

  captured_out->clear();
  char buf[2048];
  while (true) {
    const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
    if (n > 0) {
      captured_out->append(buf, static_cast<size_t>(n));
    } else {
      break;
    }
  }
  ::close(pipefd[0]);
  return true;
}

TEST(DumpTest, BufdumpHandlesNullAndAllPrimitiveAndContainerKinds) {
  ncgen_shim_reset_globals();
  const absl::Cleanup cleanup_ncgen_globals([]() {
    if (filldatalist != nullptr) {
      filldatalist->readonly = 0;
      freedatalist(filldatalist);
      filldatalist = nullptr;
    }
    bbFree(codebuffer);
    codebuffer = nullptr;
    bbFree(stmt);
    stmt = nullptr;
  });
  Bytebuffer* buf = bbNew();
  ASSERT_NE(nullptr, buf);
  const absl::Cleanup free_buf([&]() { bbFree(buf); });

  bufdump(nullptr, buf);
  EXPECT_STREQ("NULL", bbContents(buf));
  bbClear(buf);

  char enum_fqn[] = "Color_Red";
  Symbol enum_sym{};
  enum_sym.fqn = enum_fqn;

  char str_text[] = "hello";
  char opq_text[] = "deadbeef";

  NCConstant c_char_a{};
  c_char_a.nctype = NC_CHAR;
  c_char_a.value.charv = 'a';

  NCConstant c_char_esc{};
  c_char_esc.nctype = NC_CHAR;
  c_char_esc.value.charv = '\n';

  NCConstant c_byte{};
  c_byte.nctype = NC_BYTE;
  c_byte.value.int8v = -12;

  NCConstant c_short{};
  c_short.nctype = NC_SHORT;
  c_short.value.int16v = -1234;

  NCConstant c_int{};
  c_int.nctype = NC_INT;
  c_int.value.int32v = -123456;

  NCConstant c_float{};
  c_float.nctype = NC_FLOAT;
  c_float.value.floatv = 1.5F;

  NCConstant c_double{};
  c_double.nctype = NC_DOUBLE;
  c_double.value.doublev = 3.25;

  NCConstant c_ubyte{};
  c_ubyte.nctype = NC_UBYTE;
  c_ubyte.value.uint8v = 250;

  NCConstant c_ushort{};
  c_ushort.nctype = NC_USHORT;
  c_ushort.value.uint16v = 65000;

  NCConstant c_uint{};
  c_uint.nctype = NC_UINT;
  c_uint.value.uint32v = 4000000000U;

  NCConstant c_int64{};
  c_int64.nctype = NC_INT64;
  c_int64.value.int64v = -9000000000LL;

  NCConstant c_uint64{};
  c_uint64.nctype = NC_UINT64;
  c_uint64.value.uint64v = 18000000000ULL;

  NCConstant c_econst{};
  c_econst.nctype = NC_ECONST;
  c_econst.value.enumv = &enum_sym;

  NCConstant c_string{};
  c_string.nctype = NC_STRING;
  c_string.value.stringv.len = 5;
  c_string.value.stringv.stringv = str_text;

  NCConstant c_opaque{};
  c_opaque.nctype = NC_OPAQUE;
  c_opaque.value.opaquev.len = 8;
  c_opaque.value.opaquev.stringv = opq_text;

  NCConstant c_fill{};
  c_fill.nctype = NC_FILLVALUE;

  NCConstant c_unknown{};
  c_unknown.nctype = NC_NAT;

  // Inner lists for compound (both NC_DIM and NC_NAT subtypes), array, vlen.
  Datalist* inner_dim = builddatalist(2);
  Datalist* inner_cmpd = builddatalist(2);
  Datalist* inner_arr = builddatalist(2);
  Datalist* inner_vlen = builddatalist(2);
  Datalist* top = builddatalist(32);
  const absl::Cleanup free_lists([&]() {
    freedatalist(top);
    freedatalist(inner_vlen);
    freedatalist(inner_arr);
    freedatalist(inner_cmpd);
    freedatalist(inner_dim);
  });

  dlappend(inner_dim, &c_int);
  dlappend(inner_cmpd, &c_short);
  dlappend(inner_arr, &c_byte);
  dlappend(inner_vlen, &c_uint);

  NCConstant c_cmpd_dim{};
  c_cmpd_dim.nctype = NC_COMPOUND;
  c_cmpd_dim.subtype = NC_DIM;
  c_cmpd_dim.value.compoundv = inner_dim;

  NCConstant c_cmpd_struct{};
  c_cmpd_struct.nctype = NC_COMPOUND;
  c_cmpd_struct.subtype = NC_NAT;
  c_cmpd_struct.value.compoundv = inner_cmpd;

  NCConstant c_array{};
  c_array.nctype = NC_ARRAY;
  c_array.value.compoundv = inner_arr;

  NCConstant c_vlen{};
  c_vlen.nctype = NC_VLEN;
  c_vlen.value.compoundv = inner_vlen;

  NCConstant* all_consts[] = {
      &c_char_a,      &c_char_esc, &c_byte,   &c_short, &c_int,     &c_float,
      &c_double,      &c_ubyte,    &c_ushort, &c_uint,  &c_int64,   &c_uint64,
      &c_econst,      &c_string,   &c_opaque, &c_fill,  &c_unknown, &c_cmpd_dim,
      &c_cmpd_struct, &c_array,    &c_vlen,
  };
  for (NCConstant* con : all_consts) {
    dlappend(top, con);
  }

  bufdump(top, buf);
  const std::string_view out(bbContents(buf));
  EXPECT_THAT(out, ::testing::HasSubstr("'a'"));
  EXPECT_THAT(out, ::testing::HasSubstr("'\\n'"));
  EXPECT_THAT(out, ::testing::HasSubstr("-12"));
  EXPECT_THAT(out, ::testing::HasSubstr("-1234"));
  EXPECT_THAT(out, ::testing::HasSubstr("-123456"));
  EXPECT_THAT(out, ::testing::HasSubstr("1.5"));
  EXPECT_THAT(out, ::testing::HasSubstr("3.25"));
  EXPECT_THAT(out, ::testing::HasSubstr("250"));
  EXPECT_THAT(out, ::testing::HasSubstr("65000"));
  EXPECT_THAT(out, ::testing::HasSubstr("4000000000"));
  EXPECT_THAT(out, ::testing::HasSubstr("-9000000000"));
  EXPECT_THAT(out, ::testing::HasSubstr("18000000000"));
  EXPECT_THAT(out, ::testing::HasSubstr("Color_Red"));
  EXPECT_THAT(out, ::testing::HasSubstr("\"hello\""));
  EXPECT_THAT(out, ::testing::HasSubstr("0xdeadbeef"));
  EXPECT_THAT(out, ::testing::HasSubstr(" _"));
  EXPECT_THAT(out, ::testing::HasSubstr("?0? "));
  EXPECT_THAT(out, ::testing::HasSubstr("( -123456)"));
  EXPECT_THAT(out, ::testing::HasSubstr("{ -1234}"));
  EXPECT_THAT(out, ::testing::HasSubstr("[ -12]"));
  EXPECT_THAT(out, ::testing::HasSubstr("{* 4000000000}"));
}

TEST(DumpTest, DumpdatalistAndDumpconstantPrintFormattedTextToStderr) {
  ASSERT_TRUE(RunInForkedChild([]() -> bool {
    NCConstant c_int{};
    c_int.nctype = NC_INT;
    c_int.value.int32v = 42;

    Datalist* dl = builddatalist(1);
    dlappend(dl, &c_int);

    std::string list_out;
    char list_tag[] = "list_tag";
    if (!RunCapturingStderrInChild([&]() { dumpdatalist(dl, list_tag); },
                                   &list_out)) {
      freedatalist(dl);
      return false;
    }
    freedatalist(dl);

    std::string const_out;
    char const_tag[] = "const_tag";
    if (!RunCapturingStderrInChild([&]() { dumpconstant(&c_int, const_tag); },
                                   &const_out)) {
      return false;
    }
    return list_out == "list_tag:  42\n" && const_out == "const_tag:  42\n";
  }));
}

TEST(DumpTest, DumpgroupRespectsDebugThresholdAndListsSymbols) {
  ASSERT_TRUE(RunInForkedChild([]() -> bool {
    std::string low_debug_out;
    debug = 1;
    if (!RunCapturingStderrInChild([]() { dumpgroup(nullptr); },
                                   &low_debug_out)) {
      return false;
    }
    if (!low_debug_out.empty()) {
      return false;
    }

    debug = 2;
    std::string null_group_out;
    if (!RunCapturingStderrInChild([]() { dumpgroup(nullptr); },
                                   &null_group_out)) {
      return false;
    }
    if (null_group_out != "group null {\n}\n") {
      return false;
    }

    char empty_name[] = "g_empty";
    Symbol g_empty{};
    g_empty.objectclass = NC_GRP;
    g_empty.name = empty_name;
    std::string empty_group_out;
    if (!RunCapturingStderrInChild([&]() { dumpgroup(&g_empty); },
                                   &empty_group_out)) {
      return false;
    }
    if (empty_group_out != "group g_empty {\n}\n") {
      return false;
    }

    char grp_name[] = "g_main";
    char prim_name[] = "p_int";
    char type_name[] = "t_cmpd";
    char var_name[] = "v_temp";

    Symbol sym_prim{};
    sym_prim.objectclass = NC_PRIM;
    sym_prim.subclass = NC_INT;
    sym_prim.name = prim_name;

    Symbol sym_type{};
    sym_type.objectclass = NC_TYPE;
    sym_type.subclass = NC_COMPOUND;
    sym_type.name = type_name;
    sym_type.ref.is_ref = 1;

    Symbol sym_var{};
    sym_var.objectclass = NC_VAR;
    sym_var.name = var_name;

    List* subnodes = listnew();
    listpush(subnodes, &sym_prim);
    listpush(subnodes, &sym_type);
    listpush(subnodes, &sym_var);

    Symbol grp{};
    grp.objectclass = NC_GRP;
    grp.name = grp_name;
    grp.subnodes = subnodes;

    std::string full_out;
    const bool ok =
        RunCapturingStderrInChild([&]() { dumpgroup(&grp); }, &full_out);
    listfree(subnodes);
    if (!ok) {
      return false;
    }
    return full_out.find("group g_main {") != std::string::npos &&
           full_out.find("p_int\tNC_INT") != std::string::npos &&
           full_out.find("t_cmpd\tNC_COMPOUND\tref") != std::string::npos &&
           full_out.find("v_temp\tNC_VAR") != std::string::npos;
  }));
}

TEST(DumpTest, Dumpconstant1PrintsAllConstantKinds) {
  ASSERT_TRUE(RunInForkedChild([]() -> bool {
    char enum_name[] = "E_ONE";
    Symbol enum_sym{};
    enum_sym.name = enum_name;

    char str_val[] = "abc";
    char opq_val[] = "cafe";

    NCConstant c_prim{};
    c_prim.nctype = NC_INT;
    c_prim.value.int32v = 7;

    Datalist* inner = builddatalist(1);
    dlappend(inner, &c_prim);

    NCConstant c_cmpd_dim{};
    c_cmpd_dim.nctype = NC_COMPOUND;
    c_cmpd_dim.subtype = NC_DIM;
    c_cmpd_dim.value.compoundv = inner;

    NCConstant c_cmpd_other{};
    c_cmpd_other.nctype = NC_COMPOUND;
    c_cmpd_other.subtype = NC_NAT;
    c_cmpd_other.value.compoundv = inner;

    NCConstant c_str{};
    c_str.nctype = NC_STRING;
    c_str.value.stringv.len = 3;
    c_str.value.stringv.stringv = str_val;

    NCConstant c_str_empty{};
    c_str_empty.nctype = NC_STRING;

    NCConstant c_opq{};
    c_opq.nctype = NC_OPAQUE;
    c_opq.value.opaquev.len = 4;
    c_opq.value.opaquev.stringv = opq_val;

    NCConstant c_opq_empty{};
    c_opq_empty.nctype = NC_OPAQUE;

    NCConstant c_enum{};
    c_enum.nctype = NC_ECONST;
    c_enum.value.enumv = &enum_sym;

    NCConstant c_enum_null{};
    c_enum_null.nctype = NC_ECONST;

    NCConstant c_fill{};
    c_fill.nctype = NC_FILLVALUE;

    NCConstant c_char{};
    c_char.nctype = NC_CHAR;
    c_char.value.charv = 'Z';

    NCConstant c_byte{};
    c_byte.nctype = NC_BYTE;
    c_byte.value.int8v = -5;

    NCConstant c_ubyte{};
    c_ubyte.nctype = NC_UBYTE;
    c_ubyte.value.uint8v = 200;

    NCConstant c_short{};
    c_short.nctype = NC_SHORT;
    c_short.value.int16v = -300;

    NCConstant c_ushort{};
    c_ushort.nctype = NC_USHORT;
    c_ushort.value.uint16v = 50000;

    NCConstant c_uint{};
    c_uint.nctype = NC_UINT;
    c_uint.value.uint32v = 3000000000U;

    NCConstant c_int64{};
    c_int64.nctype = NC_INT64;
    c_int64.value.int64v = -12345678901LL;

    NCConstant c_uint64{};
    c_uint64.nctype = NC_UINT64;
    c_uint64.value.uint64v = 98765432101ULL;

    NCConstant c_float{};
    c_float.nctype = NC_FLOAT;
    c_float.value.floatv = 2.5F;

    NCConstant c_double{};
    c_double.nctype = NC_DOUBLE;
    c_double.value.doublev = 6.25;

    NCConstant c_unknown{};
    c_unknown.nctype = NC_NAT;

    NCConstant* inputs[] = {
        &c_cmpd_dim,  &c_cmpd_other, &c_str,       &c_str_empty, &c_opq,
        &c_opq_empty, &c_enum,       &c_enum_null, &c_fill,      &c_char,
        &c_byte,      &c_ubyte,      &c_short,     &c_ushort,    &c_prim,
        &c_uint,      &c_int64,      &c_uint64,    &c_float,     &c_double,
        &c_unknown,
    };

    std::string out;
    const bool ok = RunCapturingStderrInChild(
        [&]() {
          for (NCConstant* con : inputs) {
            dumpconstant1(con);
            std::fputc('|', stderr);
          }
        },
        &out);
    freedatalist(inner);
    if (!ok) {
      return false;
    }
    return out ==
           "{ 7}|{ 7}|\"abc\"|\"\"|0xcafe|0x--|E_ONE|?|_|'Z'|-5|200|-300|50000|"
           "7|3000000000|-12345678901|98765432101|2.5|6.25|<unknown>|";
  }));
}

}  // namespace
}  // namespace netcdf
