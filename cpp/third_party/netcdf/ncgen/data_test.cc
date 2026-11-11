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

// Unit tests for //third_party/netcdf/ncgen/data.c.
//
// Covers NCConstant/Datalist creation, cloning, insertion, subsetting,
// flattening, reclamation, commification, byte-buffer alignment, indentation,
// code-buffer formatting, and fatal assertion branches.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/data.c

#if !defined(ADDRESS_SANITIZER) && !defined(MEMORY_SANITIZER) && \
    !defined(THREAD_SANITIZER)
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#endif
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"

namespace netcdf {
namespace {

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

class DeathTestStyleGuard {
 public:
  DeathTestStyleGuard() : prev_(GTEST_FLAG_GET(death_test_style)) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
  }
  ~DeathTestStyleGuard() { GTEST_FLAG_SET(death_test_style, prev_); }

 private:
  std::string prev_;
};

#if !defined(ADDRESS_SANITIZER) && !defined(MEMORY_SANITIZER) && \
    !defined(THREAD_SANITIZER)
sigjmp_buf g_abort_jmp_buf;
volatile sig_atomic_t g_abort_jmp_active = 0;

void HandleAbortForCoverage(int /*sig*/) {
  if (g_abort_jmp_active) {
    siglongjmp(g_abort_jmp_buf, 1);
  }
}

template <typename Fn>
::testing::AssertionResult ExpectAbortInForkedChild(Fn&& fn) {
  return RunInForkedChild([&]() -> bool {
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = HandleAbortForCoverage;
    sigemptyset(&sa.sa_mask);
    struct sigaction prev_sa;
    std::memset(&prev_sa, 0, sizeof(prev_sa));
    if (::sigaction(SIGABRT, &sa, &prev_sa) != 0) {
      return false;
    }
    g_abort_jmp_active = 1;
    if (sigsetjmp(g_abort_jmp_buf, 1) == 0) {
      std::forward<Fn>(fn)();
      g_abort_jmp_active = 0;
      ::sigaction(SIGABRT, &prev_sa, nullptr);
      return false;
    }
    g_abort_jmp_active = 0;
    ::sigaction(SIGABRT, &prev_sa, nullptr);
    return true;
  });
}
#endif

class DataTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CleanupNcgenGlobals();
    ncgen_shim_reset_globals();
  }

  void TearDown() override {
    usingclassic = 0;
    debug = 0;
    settrace(0);
    CleanupNcgenGlobals();
  }
};

template <typename T>
void ZeroStruct(T* obj) {
  std::memset(obj, 0, sizeof(T));
}

void InitConstant(NCConstant* con, nc_type nctype = NC_NAT) {
  ZeroStruct(con);
  con->nctype = nctype;
}

NCConstant* MakeHeapIntConst(int val, int lineno) {
  NCConstant* c = nullconst();
  c->nctype = NC_INT;
  c->lineno = lineno;
  c->value.int32v = val;
  return c;
}

TEST_F(DataTest, ConstantsStringableCreationCloningAndClearing) {
  EXPECT_EQ(1, isstringable(NC_CHAR));
  EXPECT_EQ(1, isstringable(NC_STRING));
  EXPECT_EQ(1, isstringable(NC_BYTE));
  EXPECT_EQ(1, isstringable(NC_UBYTE));
  EXPECT_EQ(1, isstringable(NC_FILLVALUE));
  EXPECT_EQ(0, isstringable(NC_INT));
  EXPECT_EQ(0, isstringable(NC_DOUBLE));
  EXPECT_EQ(0, isstringable(NC_NAT));

  NCConstant* emp_cmpd = emptycompoundconst(12);
  ASSERT_NE(nullptr, emp_cmpd);
  EXPECT_EQ(NC_COMPOUND, emp_cmpd->nctype);
  EXPECT_EQ(12, emp_cmpd->lineno);
  EXPECT_EQ(0UL, datalistlen(emp_cmpd->value.compoundv));
  reclaimconstant(emp_cmpd);

  NCConstant* emp_str = emptystringconst(34);
  ASSERT_NE(nullptr, emp_str);
  EXPECT_EQ(NC_STRING, emp_str->nctype);
  EXPECT_EQ(34, emp_str->lineno);
  EXPECT_EQ(0UL, emp_str->value.stringv.len);
  EXPECT_EQ(nullptr, emp_str->value.stringv.stringv);
  reclaimconstant(emp_str);

  // const2list and list2const (writable and readonly paths).
  NCConstant* item = MakeHeapIntConst(99, 77);
  Datalist* dl = const2list(item);
  ASSERT_NE(nullptr, dl);
  EXPECT_EQ(77, datalistline(dl));
  NCConstant* cmpd_const = list2const(dl);
  ASSERT_NE(nullptr, cmpd_const);
  EXPECT_EQ(NC_COMPOUND, cmpd_const->nctype);
  EXPECT_EQ(77, cmpd_const->lineno);
  reclaimconstant(cmpd_const);

  NCConstant* ro_const = list2const(filldatalist);
  ASSERT_NE(nullptr, ro_const);
  EXPECT_EQ(0, ro_const->lineno);
  // filldatalist is owned globally; free only the wrapper NCConstant shallowly.
  freeconstant(ro_const, TOPLEVEL);

  // cloneconstant across string (empty and non-empty), opaque (empty and
  // non-empty), compound (null and non-null), and scalar types.
  NCConstant str_empty;
  InitConstant(&str_empty, NC_STRING);
  NCConstant* str_empty_clone = cloneconstant(&str_empty);
  ASSERT_NE(nullptr, str_empty_clone);
  EXPECT_EQ(nullptr, str_empty_clone->value.stringv.stringv);
  freeconst(str_empty_clone);

  NCConstant* str_full = nullconst();
  str_full->nctype = NC_STRING;
  str_full->value.stringv.len = 5;
  str_full->value.stringv.stringv = chkstrdup("alpha");
  NCConstant* str_full_clone = cloneconstant(str_full);
  ASSERT_NE(nullptr, str_full_clone);
  EXPECT_STREQ("alpha", str_full_clone->value.stringv.stringv);
  EXPECT_NE(str_full->value.stringv.stringv,
            str_full_clone->value.stringv.stringv);
  reclaimconstant(str_full);
  reclaimconstant(str_full_clone);

  NCConstant opq_empty;
  InitConstant(&opq_empty, NC_OPAQUE);
  NCConstant* opq_empty_clone = cloneconstant(&opq_empty);
  ASSERT_NE(nullptr, opq_empty_clone);
  EXPECT_STREQ("", opq_empty_clone->value.opaquev.stringv);
  reclaimconstant(opq_empty_clone);

  NCConstant* opq_null = nullconst();
  opq_null->nctype = NC_OPAQUE;
  reclaimconstant(opq_null);

  NCConstant* opq_full = nullconst();
  opq_full->nctype = NC_OPAQUE;
  opq_full->value.opaquev.len = 4;
  opq_full->value.opaquev.stringv = chkstrdup("beef");
  NCConstant* opq_full_clone = cloneconstant(opq_full);
  ASSERT_NE(nullptr, opq_full_clone);
  EXPECT_STREQ("beef", opq_full_clone->value.opaquev.stringv);
  reclaimconstant(opq_full);
  reclaimconstant(opq_full_clone);

  NCConstant cmpd_null;
  InitConstant(&cmpd_null, NC_COMPOUND);
  NCConstant* cmpd_null_clone = cloneconstant(&cmpd_null);
  ASSERT_NE(nullptr, cmpd_null_clone);
  EXPECT_EQ(nullptr, cmpd_null_clone->value.compoundv);
  reclaimconstant(cmpd_null_clone);

  NCConstant* cmpd_src = emptycompoundconst(5);
  dlappend(cmpd_src->value.compoundv, MakeHeapIntConst(11, 5));
  NCConstant* cmpd_clone = cloneconstant(cmpd_src);
  ASSERT_NE(nullptr, cmpd_clone);
  EXPECT_EQ(1UL, datalistlen(cmpd_clone->value.compoundv));
  EXPECT_EQ(11, datalistith(cmpd_clone->value.compoundv, 0)->value.int32v);
  reclaimconstant(cmpd_src);
  reclaimconstant(cmpd_clone);

  // clearconstant and freeconstant edge cases.
  clearconstant(nullptr);
  NCConstant stack_str;
  InitConstant(&stack_str, NC_STRING);
  stack_str.value.stringv.len = 2;
  stack_str.value.stringv.stringv = chkstrdup("hi");
  clearconstant(&stack_str);
  EXPECT_EQ(NC_NAT, stack_str.nctype);

  NCConstant stack_str_null;
  InitConstant(&stack_str_null, NC_STRING);
  clearconstant(&stack_str_null);

  NCConstant stack_opq;
  InitConstant(&stack_opq, NC_OPAQUE);
  stack_opq.value.opaquev.len = 2;
  stack_opq.value.opaquev.stringv = chkstrdup("aa");
  clearconstant(&stack_opq);

  NCConstant stack_opq_null;
  InitConstant(&stack_opq_null, NC_OPAQUE);
  clearconstant(&stack_opq_null);

  NCConstant stack_cmpd;
  InitConstant(&stack_cmpd, NC_COMPOUND);
  stack_cmpd.value.compoundv = filldatalist;
  clearconstant(&stack_cmpd);
  EXPECT_EQ(nullptr, stack_cmpd.value.compoundv);

  NCConstant* scalar_con = MakeHeapIntConst(7, 8);
  freeconst(scalar_con);

  NCConstant* shallow_con = MakeHeapIntConst(1, 2);
  freeconstant(shallow_con, TOPLEVEL);
  freeconstant(nullptr, DEEP);

  EXPECT_EQ(0, datalistline(nullptr));
  Datalist* empty_dl = builddatalist(0);
  EXPECT_EQ(0, datalistline(empty_dl));
  reclaimdatalist(empty_dl);
}

TEST_F(DataTest, DatalistExtendInsertSetRemoveSubsetCloneAndFlatten) {
  // Exercise dlextend() when initial alloc == 0.
  Datalist raw0;
  ZeroStruct(&raw0);
  dlextend(&raw0);
  EXPECT_EQ(2UL, raw0.alloc);
  efree(raw0.data);

  Datalist* dl = builddatalist(2);
  ASSERT_NE(nullptr, dl);
  EXPECT_EQ(3UL, dl->alloc);

  // No-op dlsetalloc when capacity already suffices, and grow when empty.
  dlsetalloc(dl, 2);
  EXPECT_EQ(3UL, dl->alloc);
  dlsetalloc(dl, 4);
  EXPECT_EQ(4UL, dl->alloc);

  for (int i = 0; i < 6; ++i) {
    dlappend(dl, MakeHeapIntConst(i * 10, i + 1));
  }
  EXPECT_EQ(6UL, datalistlen(dl));
  EXPECT_GE(dl->alloc, 8UL);

  NCConstant* replacement = MakeHeapIntConst(99, 9);
  NCConstant* old = datalistith(dl, 1);
  dlset(dl, 1, replacement);
  reclaimconstant(old);
  EXPECT_EQ(99, datalistith(dl, 1)->value.int32v);

  NCConstant* removed = dlremove(dl, 1);
  EXPECT_EQ(replacement, removed);
  reclaimconstant(removed);
  EXPECT_EQ(5UL, datalistlen(dl));

  // dlinsert in middle (delta > 0) and at tail (delta == 0).
  Datalist* ins = builddatalist(2);
  dlappend(ins, MakeHeapIntConst(100, 10));
  dlappend(ins, MakeHeapIntConst(200, 11));
  dlinsert(dl, 1, ins);
  EXPECT_EQ(7UL, datalistlen(dl));
  EXPECT_EQ(100, datalistith(dl, 1)->value.int32v);
  EXPECT_EQ(200, datalistith(dl, 2)->value.int32v);
  dlinsert(dl, datalistlen(dl), ins);
  EXPECT_EQ(9UL, datalistlen(dl));
  reclaimdatalist(ins);

  // builddatasubset null/OOB/clamped/normal cases.
  EXPECT_EQ(nullptr, builddatasubset(nullptr, 0, 1));
  EXPECT_EQ(nullptr, builddatasubset(dl, datalistlen(dl), 1));
  Datalist* clamped = builddatasubset(dl, 7, 100);
  ASSERT_NE(nullptr, clamped);
  EXPECT_EQ(1, clamped->readonly);
  EXPECT_EQ(2UL, datalistlen(clamped));
  EXPECT_EQ(100, datalistith(clamped, 0)->value.int32v);
  reclaimdatalist(clamped);

  Datalist* mid = builddatasubset(dl, 1, 2);
  ASSERT_NE(nullptr, mid);
  EXPECT_EQ(2UL, datalistlen(mid));
  freedatalist(mid);

  // clonedatalist on NULL and non-NULL.
  EXPECT_EQ(nullptr, clonedatalist(nullptr));
  Datalist* dl_clone = clonedatalist(dl);
  ASSERT_NE(nullptr, dl_clone);
  EXPECT_EQ(datalistlen(dl), datalistlen(dl_clone));
  reclaimdatalist(dl_clone);

  // builddatasublist on empty and non-empty lists.
  Datalist* empty_sub = builddatalist(1);
  NCConstant* sub_empty_con = builddatasublist(empty_sub);
  EXPECT_EQ(0, sub_empty_con->lineno);
  reclaimconstant(sub_empty_con);

  NCConstant* sub_nonempty_con = builddatasublist(dl);
  EXPECT_EQ(1, sub_nonempty_con->lineno);
  // Detach dl before freeing sub_nonempty_con if testing separately, or let
  // reclaimconstant(sub_nonempty_con) free dl cleanly.
  reclaimconstant(sub_nonempty_con);

  // reclaimdatalist with NULL, NULL slot, and readonly NULL data pointer.
  // Note: freedatalist() unconditionally calls efree(list->data) when
  // !list->readonly, so a non-readonly Datalist with data == NULL panics in
  // chkfree(NULL).
  reclaimdatalist(nullptr);
  reclaimconstant(nullptr);
  freedatalist(nullptr);
  Datalist* sparse = builddatalist(2);
  dlappend(sparse, nullptr);
  reclaimdatalist(sparse);

  Datalist* readonly_empty = static_cast<Datalist*>(ecalloc(sizeof(Datalist)));
  readonly_empty->readonly = 1;
  reclaimdatalist(readonly_empty);

  // flatten() across rank == 0, empty list, nested compounds, and short depth.
  Datalist* nested = builddatalist(3);
  NCConstant* row1 = emptycompoundconst(1);
  dlappend(row1->value.compoundv, MakeHeapIntConst(1, 1));
  dlappend(row1->value.compoundv, MakeHeapIntConst(2, 1));
  dlappend(nested, row1);
  dlappend(nested, MakeHeapIntConst(3, 2));  // short-depth scalar at depth 0

  Datalist* flat0 = flatten(nested, 0);
  EXPECT_EQ(0UL, datalistlen(flat0));
  freedatalist(flat0);

  Datalist* empty_in = builddatalist(1);
  Datalist* flat_empty = flatten(empty_in, 2);
  EXPECT_EQ(0UL, datalistlen(flat_empty));
  freedatalist(flat_empty);
  reclaimdatalist(empty_in);

  Datalist* flat2 = flatten(nested, 2);
  ASSERT_EQ(3UL, datalistlen(flat2));
  EXPECT_EQ(1, datalistith(flat2, 0)->value.int32v);
  EXPECT_EQ(2, datalistith(flat2, 1)->value.int32v);
  EXPECT_EQ(3, datalistith(flat2, 2)->value.int32v);
  freedatalist(flat2);
  reclaimdatalist(nested);
}

TEST_F(DataTest, CaptureAndReclaimAllDatalistsInForkedChild) {
  // reclaimalldatalists() leaks alldatalists->content upstream, so run inside
  // an isolated forked child to preserve coverage cleanly under LSan.
  ASSERT_TRUE(RunInForkedChild([]() -> bool {
    Datalist* d1 = builddatalist(1);
    dlappend(d1, MakeHeapIntConst(5, 1));
    Datalist* d2 = builddatalist(1);
    dlappend(d2, MakeHeapIntConst(6, 2));
    capture(d1);
    capture(d2);
    capture(nullptr);
    if (alldatalists == nullptr || listlength(alldatalists) != 3UL) {
      return false;
    }
    reclaimalldatalists();
    return alldatalists == nullptr;
  }));
}

TEST_F(DataTest, CommifyAndWordNormalizeDelimitersQuotesAndBraces) {
  Bytebuffer* buf = bbNew();
  ASSERT_NE(nullptr, buf);
  const absl::Cleanup free_buf([&]() { bbFree(buf); });

  // Empty buffer is a no-op.
  commify(buf);
  EXPECT_EQ(0U, bbLength(buf));

  bbCat(buf, "1 2, 3 { 4 5 { 6 7 } } 'a' \"hi \\\"x\\\"\" esc\\ y");
  commify(buf);
  const std::string first_pass(bbContents(buf));
  EXPECT_EQ("1, 2, 3, {4, 5, {6, 7}}, 'a', \"hi \\\"x\\\"\", esc\\ y",
            first_pass);

  // Commify is idempotent on already-commified input.
  commify(buf);
  EXPECT_EQ(first_pass, std::string_view(bbContents(buf)));

  // Empty braces and trailing backslash before NUL inside wordstring().
  bbClear(buf);
  bbCat(buf, "{} \"abc\\");
  commify(buf);
  EXPECT_STREQ("{}, \"abc\\", bbContents(buf));

  // Direct word() calls covering delimiters and trailing backslash.
  Bytebuffer* wbuf = bbNew();
  ASSERT_NE(nullptr, wbuf);
  const absl::Cleanup free_wbuf([&]() { bbFree(wbuf); });

  char input1[] = "foo\\ bar}tail";
  char* rem1 = word(input1, wbuf);
  EXPECT_STREQ("foo\\ bar", bbContents(wbuf));
  EXPECT_EQ('}', *rem1);

  bbClear(wbuf);
  char input2[] = "tok,next";
  char* rem2 = word(input2, wbuf);
  EXPECT_STREQ("tok", bbContents(wbuf));
  EXPECT_EQ(',', *rem2);

  bbClear(wbuf);
  char input3[] = "end\\";
  char* rem3 = word(input3, wbuf);
  EXPECT_STREQ("end\\", bbContents(wbuf));
  EXPECT_EQ('\0', *rem3);
}

TEST_F(DataTest, AlignbufferPadsOffsetsByTypeAndClassicMode) {
  Bytebuffer* buf = bbNew();
  ASSERT_NE(nullptr, buf);
  const absl::Cleanup free_buf([&]() { bbFree(buf); });

  NCConstant c_char;
  InitConstant(&c_char, NC_CHAR);
  bbAppend(buf, 'x');
  alignbuffer(&c_char, buf);
  EXPECT_EQ(1U, bbLength(buf));

  NCConstant c_int;
  InitConstant(&c_int, NC_INT);
  alignbuffer(&c_int, buf);
  EXPECT_EQ(4U, bbLength(buf));
  // Already aligned -> pad == 0.
  alignbuffer(&c_int, buf);
  EXPECT_EQ(4U, bbLength(buf));

  NCConstant c_double;
  InitConstant(&c_double, NC_DOUBLE);
  alignbuffer(&c_double, buf);
  EXPECT_EQ(8U, bbLength(buf));

  Symbol enum_sym;
  ZeroStruct(&enum_sym);
  enum_sym.typ.typecode = NC_INT;
  NCConstant c_enum;
  InitConstant(&c_enum, NC_ECONST);
  c_enum.value.enumv = &enum_sym;
  bbAppend(buf, 'y');
  alignbuffer(&c_enum, buf);
  EXPECT_EQ(12U, bbLength(buf));

  NCConstant c_str;
  InitConstant(&c_str, NC_STRING);
  usingclassic = 1;
  bbAppend(buf, 'z');
  alignbuffer(&c_str, buf);
  EXPECT_EQ(13U, bbLength(buf));

  usingclassic = 0;
  alignbuffer(&c_str, buf);
  EXPECT_EQ(16U, bbLength(buf));

  // Unalignable type (NC_NAT) leaves length unchanged.
  NCConstant c_nat;
  InitConstant(&c_nat, NC_NAT);
  bbAppend(buf, 'w');
  alignbuffer(&c_nat, buf);
  EXPECT_EQ(17U, bbLength(buf));
}

TEST_F(DataTest, IndentBbprintfAndCodeOutputFormatCorrectly) {
  EXPECT_STREQ("", indented(0));
  EXPECT_STREQ("    ", indented(1));
  EXPECT_EQ(256UL, std::strlen(indented(100)));

  Bytebuffer* buf = bbNew();
  ASSERT_NE(nullptr, buf);
  const absl::Cleanup free_buf([&]() { bbFree(buf); });

  bbindent(buf, 2);
  EXPECT_STREQ("        ", bbContents(buf));

  char txt_arg[] = "txt";
  bbprintf0(buf,
            "%% %u %hu %hhu %hhhu %lu %llu %lllu | %d %hd %hhd %ld %lld | "
            "%f %lf | %s %c end%",
            static_cast<unsigned int>(10U), static_cast<unsigned int>(20U),
            static_cast<unsigned int>(30U), static_cast<unsigned int>(40U),
            50UL, 60ULL, 70ULL, -1, -2, -3, -4L, -5LL, 1.5, 2.25, txt_arg,
            static_cast<int>('Q'));
  const std::string_view formatted(bbContents(buf));
  EXPECT_THAT(formatted,
              ::testing::HasSubstr("% 10 20 30 40 50 60 70 | -1 -2 -3 -4 -5"));
  EXPECT_THAT(formatted,
              ::testing::HasSubstr("((float)1.5) ((double)2.25) | txt Q end%"));

  bbClear(buf);
  bbprintf(buf, "part1");
  bbprintf(buf, "+part2");
  EXPECT_STREQ("part1+part2", bbContents(buf));

  codepartial("head");
  codeline(";");
  codelined(1, "body;");
  codeprintf("val=%d\n", 9);
  codedump(buf);
  EXPECT_EQ(0U, bbLength(buf));
  EXPECT_STREQ("head;\n    body;\nval=9\npart1+part2", bbContents(codebuffer));

  // Verify codeflush() on both non-empty and empty codebuffer via pipe.
  ASSERT_TRUE(RunInForkedChild([]() -> bool {
    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) != 0) {
      return false;
    }
    const int saved_stdout = ::dup(STDOUT_FILENO);
    if (saved_stdout < 0) {
      ::close(pipefd[0]);
      ::close(pipefd[1]);
      return false;
    }
    std::fflush(stdout);
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::close(pipefd[1]);

    codeflush();
    // Second call on empty codebuffer should be a no-op.
    codeflush();

    ::dup2(saved_stdout, STDOUT_FILENO);
    ::close(saved_stdout);

    std::string captured;
    char tmp[256];
    while (true) {
      const ssize_t n = ::read(pipefd[0], tmp, sizeof(tmp));
      if (n > 0) {
        captured.append(tmp, static_cast<size_t>(n));
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else {
        break;
      }
    }
    ::close(pipefd[0]);
    return bbLength(codebuffer) == 0U &&
           captured == "head;\n    body;\nval=9\npart1+part2";
  }));
}

TEST(DataDeathTest, FatalAssertionsAndReadonlyGuardsAbort) {
  DeathTestStyleGuard death_guard;

  auto run_death_checks = []() {
    EXPECT_DEATH(list2const(nullptr), "list != NULL");
    EXPECT_DEATH(const2list(nullptr), "con != NULL");
    EXPECT_DEATH(
        {
          Datalist ro;
          ZeroStruct(&ro);
          ro.readonly = 1;
          dlsetalloc(&ro, 4);
        },
        "");
    EXPECT_DEATH(
        {
          Datalist ro;
          ZeroStruct(&ro);
          ro.readonly = 1;
          dlextend(&ro);
        },
        "");
    EXPECT_DEATH(
        {
          Datalist ro;
          ZeroStruct(&ro);
          ro.readonly = 1;
          NCConstant c;
          InitConstant(&c);
          dlappend(&ro, &c);
        },
        "");
    EXPECT_DEATH(
        {
          Datalist dl;
          ZeroStruct(&dl);
          NCConstant c;
          InitConstant(&c);
          dlset(&dl, 0, &c);
        },
        "pos < dl->length");
    EXPECT_DEATH(
        {
          Datalist dl;
          ZeroStruct(&dl);
          dlremove(&dl, 0);
        },
        "dl->length > 0");
    EXPECT_DEATH(
        {
          NCConstant c;
          InitConstant(&c, NC_COMPOUND);
          Bytebuffer b;
          ZeroStruct(&b);
          alignbuffer(&c, &b);
        },
        "NC_COMPOUND");
#ifndef NDEBUG
    EXPECT_DEATH(
        {
          Bytebuffer b;
          ZeroStruct(&b);
          bbprintf(&b, "%q");  // NOLINT(runtime/printf_format)
        },
        "unknown specifier");
#endif
  };
  run_death_checks();

#if !defined(ADDRESS_SANITIZER) && !defined(MEMORY_SANITIZER) && \
    !defined(THREAD_SANITIZER)
  EXPECT_TRUE(ExpectAbortInForkedChild([]() { list2const(nullptr); }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() { const2list(nullptr); }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    Datalist ro;
    ZeroStruct(&ro);
    ro.readonly = 1;
    dlsetalloc(&ro, 4);
  }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    Datalist ro;
    ZeroStruct(&ro);
    ro.readonly = 1;
    dlextend(&ro);
  }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    Datalist ro;
    ZeroStruct(&ro);
    ro.readonly = 1;
    NCConstant c;
    InitConstant(&c);
    dlappend(&ro, &c);
  }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    Datalist dl;
    ZeroStruct(&dl);
    NCConstant c;
    InitConstant(&c);
    dlset(&dl, 0, &c);
  }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    Datalist dl;
    ZeroStruct(&dl);
    dlremove(&dl, 0);
  }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    NCConstant c;
    InitConstant(&c, NC_COMPOUND);
    Bytebuffer b;
    ZeroStruct(&b);
    alignbuffer(&c, &b);
  }));
#ifndef NDEBUG
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    Bytebuffer b;
    ZeroStruct(&b);
    bbprintf(&b, "%q");  // NOLINT(runtime/printf_format)
  }));
#endif
#endif
}

}  // namespace
}  // namespace netcdf
