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
//
// Tests internal API call tracing and argument formatting routines
// (third_party/hdf5/src/H5trace.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5trace.c

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5TRACE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5I_FRIEND
#include "third_party/hdf5/src/H5Ipkg.h"
#include "third_party/hdf5/src/H5RSprivate.h"
}  // extern "C"
#ifdef H5TRACE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TRACE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

class TraceCapture {
 public:
  TraceCapture(bool ttop, bool ttimes)
      : prev_trace_(H5_debug_g.trace),
        prev_ttop_(H5_debug_g.ttop),
        prev_ttimes_(H5_debug_g.ttimes) {
    stream_ = ::open_memstream(&buf_, &size_);
    EXPECT_NE(stream_, nullptr);
    H5_debug_g.trace = stream_;
    H5_debug_g.ttop = ttop;
    H5_debug_g.ttimes = ttimes;
  }

  ~TraceCapture() { Finish(); }

  std::string Finish() {
    std::string out;
    if (!restored_) {
      H5_debug_g.trace = prev_trace_;
      H5_debug_g.ttop = prev_ttop_;
      H5_debug_g.ttimes = prev_ttimes_;
      restored_ = true;
    }
    if (stream_ != nullptr) {
      ::fclose(stream_);
      stream_ = nullptr;
    }
    if (buf_ != nullptr) {
      out.assign(buf_, size_);
      std::free(buf_);
      buf_ = nullptr;
      size_ = 0;
    }
    return out;
  }

  TraceCapture(const TraceCapture&) = delete;
  TraceCapture& operator=(const TraceCapture&) = delete;

 private:
  FILE* prev_trace_;
  bool prev_ttop_;
  bool prev_ttimes_;
  FILE* stream_ = nullptr;
  char* buf_ = nullptr;
  size_t size_ = 0;
  bool restored_ = false;
};

herr_t EvalTraceArgs(std::string* formatted, const char* type, ...) {
  H5RS_str_t* rs = H5RS_create(nullptr);
  EXPECT_NE(rs, nullptr);
  absl::Cleanup decr_rs = [rs]() {
    if (rs != nullptr) {
      H5RS_decr(rs);
    }
  };

  va_list ap;
  va_start(ap, type);
  const herr_t rc = H5_trace_args(rs, type, ap);
  va_end(ap);

  const char* s = H5RS_get_str(rs);
  *formatted = (s != nullptr) ? std::string(s) : std::string();
  return rc;
}

std::string FormatArgs(const char* type, ...) {
  H5RS_str_t* rs = H5RS_create(nullptr);
  EXPECT_NE(rs, nullptr);
  absl::Cleanup decr_rs = [rs]() {
    if (rs != nullptr) {
      H5RS_decr(rs);
    }
  };

  va_list ap;
  va_start(ap, type);
  const herr_t rc = H5_trace_args(rs, type, ap);
  va_end(ap);
  EXPECT_GE(rc, 0) << "type=" << type;

  const char* s = H5RS_get_str(rs);
  return (s != nullptr) ? std::string(s) : std::string();
}

hid_t MakeSyntheticId(H5I_type_t type, uint64_t index) {
  return (static_cast<hid_t>(type) << ID_BITS) |
         (static_cast<hid_t>(index) & ID_MASK);
}

TEST(H5traceTest, TraceDisabledReturnsZero) {
  FILE* prev_trace = H5_debug_g.trace;
  H5_debug_g.trace = nullptr;
  absl::Cleanup restore = [prev_trace]() { H5_debug_g.trace = prev_trace; };

  const double t = H5_trace(nullptr, "H5Dummy", "Is", "x", 7);
  EXPECT_EQ(t, 0.0);
}

TEST(H5traceTest, TraceNormalNestedAndTimingFlow) {
  ASSERT_GE(H5open(), 0);

  // Without ttimes: single call plus 3-level nesting so exits exercise
  // both (current_depth == last_call_depth) and (current_depth < last_call_depth)
  // including the `for (i = 0; i < current_depth; i++) H5RS_aputc(rs, '+')`.
  {
    TraceCapture cap(/*ttop=*/false, /*ttimes=*/false);
    double t0 = H5_trace(nullptr, "H5Outer", "Is", "a", 1);
    double t1 = H5_trace(nullptr, "H5Mid", "Is", "b", 2);
    double t2 = H5_trace(nullptr, "H5Inner", "Is", "c", 3);
    H5_trace(&t2, "H5Inner", "e", nullptr, herr_t{0});
    H5_trace(&t1, "H5Mid", "e", nullptr, herr_t{0});
    H5_trace(&t0, "H5Outer", "e", nullptr, herr_t{0});
    // Also do a standalone enter/exit pair after depth resets.
    double t3 = H5_trace(nullptr, "H5Flat", "b", "flag", 1);
    H5_trace(&t3, "H5Flat", "e", nullptr, herr_t{-1});

    const std::string output = cap.Finish();
    EXPECT_THAT(output, HasSubstr("H5Outer(a=1) = <delayed>\n"));
    EXPECT_THAT(output, HasSubstr("+  H5Mid(b=2) = <delayed>\n"));
    EXPECT_THAT(output, HasSubstr("++    H5Inner(c=3) = SUCCEED;\n"));
    EXPECT_THAT(output, HasSubstr("+  H5Mid = SUCCEED;\n"));
    EXPECT_THAT(output, HasSubstr("H5Outer = SUCCEED;\n"));
    EXPECT_THAT(output, HasSubstr("H5Flat(flag=TRUE) = FAIL;\n"));
  }

  // With ttimes=true: exercises first-invocation timer setup, timestamps,
  // delayed nested calls, and returning into outer frame alignment spacing.
  {
    TraceCapture cap(/*ttop=*/false, /*ttimes=*/true);
    double t0 = H5_trace(nullptr, "H5TimedOuter", "Is", "a", 10);
    double t1 = H5_trace(nullptr, "H5TimedMid", "Is", "b", 20);
    double t2 = H5_trace(nullptr, "H5TimedInner", "Is", "c", 30);
    H5_trace(&t2, "H5TimedInner", "e", nullptr, herr_t{0});
    H5_trace(&t1, "H5TimedMid", "e", nullptr, herr_t{0});
    H5_trace(&t0, "H5TimedOuter", "e", nullptr, herr_t{0});

    double t3 = H5_trace(nullptr, "H5TimedFlat", "b", "flag", 0);
    H5_trace(&t3, "H5TimedFlat", "e", nullptr, herr_t{0});

    const std::string output = cap.Finish();
    EXPECT_THAT(output, HasSubstr("H5TimedOuter(a=10) = <delayed>\n"));
    EXPECT_THAT(output, HasSubstr("H5TimedMid = SUCCEED"));
    EXPECT_THAT(output, HasSubstr("[dt="));
    EXPECT_THAT(output, HasSubstr("H5TimedFlat(flag=FALSE) = SUCCEED"));
  }
}

TEST(H5traceTest, TraceTopOnlySuppressesNestedCalls) {
  ASSERT_GE(H5open(), 0);

  TraceCapture cap(/*ttop=*/true, /*ttimes=*/true);
  double t0 = H5_trace(nullptr, "H5TopLevel", "Is", "k", 5);
  double t1 = H5_trace(nullptr, "H5NestedSuppressed", "Is", "m", 6);
  EXPECT_EQ(t1, 0.0);
  H5_trace(&t1, "H5NestedSuppressed", "e", nullptr, herr_t{0});
  H5_trace(&t0, "H5TopLevel", "e", nullptr, herr_t{0});

  const std::string output = cap.Finish();
  EXPECT_THAT(output, HasSubstr("H5TopLevel(k=5) = SUCCEED"));
  EXPECT_THAT(output, Not(HasSubstr("H5NestedSuppressed")));
}

TEST(H5traceTest, FormatsPointersAndArrays) {
  hsize_t h_arr[2] = {H5S_UNLIMITED, 42};
  hssize_t hs_arr[2] = {-7, 9};
  int is_arr[2] = {-11, 22};
  unsigned iu_arr[2] = {33u, 44u};
  unsigned long ul_arr[2] = {55ul, 66ul};
  unsigned long long ull_arr[2] = {77ull, 88ull};
  int dummy_a = 1;
  void* x_arr[2] = {&dummy_a, nullptr};
  size_t z_arr[2] = {100u, 200u};
  ssize_t zs_arr[2] = {-5, 5};
  const char* str = "hello";
  const char** str_ptr = &str;
  double dbl = 3.25;

  // Arrays with positive asize_idx references.
  EXPECT_THAT(FormatArgs("Is*[a0]h*[a0]Hs*[a0]Is*[a0]Iu*[a0]Ul*[a0]UL*[a0]x*"
                         "[a0]z*[a0]Zs",
                         "n", 2, "h", h_arr, "hs", hs_arr, "is", is_arr, "iu",
                         iu_arr, "ul", ul_arr, "ull", ull_arr, "x", x_arr, "z",
                         z_arr, "zs", zs_arr),
              HasSubstr("{H5S_UNLIMITED, 42}"));
  EXPECT_THAT(FormatArgs("Is*[a0]Hs", "n", 2, "hs", hs_arr),
              HasSubstr("{-7, 9}"));
  EXPECT_THAT(FormatArgs("Is*[a0]Is", "n", 2, "is", is_arr),
              HasSubstr("{-11, 22}"));
  EXPECT_THAT(FormatArgs("Iu*[a0]Iu", "n", 2, "iu", iu_arr),
              HasSubstr("{33, 44}"));
  EXPECT_THAT(FormatArgs("Ul*[a0]Ul", "n", 2, "ul", ul_arr),
              HasSubstr("{55, 66}"));
  EXPECT_THAT(FormatArgs("UL*[a0]UL", "n", 2, "ull", ull_arr),
              HasSubstr("{77, 88}"));
  EXPECT_THAT(FormatArgs("z*[a0]x", "n", static_cast<size_t>(2), "x", x_arr),
              HasSubstr(", NULL}"));
  EXPECT_THAT(FormatArgs("Zs*[a0]z", "n", static_cast<ssize_t>(2), "z", z_arr),
              HasSubstr("{100, 200}"));
  EXPECT_THAT(FormatArgs("h*[a0]Zs", "n", static_cast<hsize_t>(2), "zs", zs_arr),
              HasSubstr("{-5, 5}"));

  // Arrays where asize < 0, fixed non-[aN] brackets, plain pointers, non-array
  // second letters under H/I/U/Z, strings with ptr==1 and ptr>1, default *d,
  // and NULL pointer.
  EXPECT_THAT(FormatArgs("Is*[a0]h*[a0]Hs*[a0]Is*[a0]Iu*[a0]Ul*[a0]UL*[a0]x*"
                         "[a0]z*[a0]Zs",
                         "neg", -1, "h", h_arr, "hs", hs_arr, "is", is_arr,
                         "iu", iu_arr, "ul", ul_arr, "ull", ull_arr, "x",
                         x_arr, "z", z_arr, "zs", zs_arr),
              Not(HasSubstr("{")));
  EXPECT_THAT(FormatArgs("*[2]Is*h*Hs*Hc*Is*Iu*It*s**s*Ul*UL*Ux*x*z*Zs*Zf*d*h",
                         "b", is_arr, "h", h_arr, "hs", hs_arr, "hc", &dummy_a,
                         "is", is_arr, "iu", iu_arr, "it", &dummy_a, "s1", str,
                         "s2", str_ptr, "ul", ul_arr, "ull", ull_arr, "ux",
                         &dummy_a, "x", &dummy_a, "z", z_arr, "zs", zs_arr,
                         "zf", &dummy_a, "d", &dbl, "nullp", nullptr),
              HasSubstr("s1=\"hello\""));
  EXPECT_EQ(FormatArgs("*h", "nullp", nullptr), "nullp=NULL");
}

TEST(H5traceTest, FormatsScalarPrimitivesAndSpecialCodes) {
  int dummy = 42;
  EXPECT_EQ(FormatArgs("a", "addr", static_cast<haddr_t>(1234)), "addr=1234");
  EXPECT_EQ(FormatArgs("a", "addr", HADDR_UNDEF), "addr=UNDEF");
  EXPECT_EQ(FormatArgs("b", "bt", 1), "bt=TRUE");
  EXPECT_EQ(FormatArgs("b", "bf", 0), "bf=FALSE");
  EXPECT_EQ(FormatArgs("d", "dbl", 1.5), "dbl=1.5");
  EXPECT_EQ(FormatArgs("e", "ok", herr_t{0}), "ok=SUCCEED");
  EXPECT_EQ(FormatArgs("e", "bad", herr_t{-1}), "bad=FAIL");
  EXPECT_EQ(FormatArgs("h", "unlim", H5S_UNLIMITED), "unlim=H5S_UNLIMITED");
  EXPECT_EQ(FormatArgs("h", "sz", static_cast<hsize_t>(99)), "sz=99");
  EXPECT_EQ(FormatArgs("Hs", "hss", static_cast<hssize_t>(-17)), "hss=-17");
  EXPECT_EQ(FormatArgs("o", "off", static_cast<off_t>(4096)), "off=4096");
  EXPECT_EQ(FormatArgs("t", "tpos", htri_t{1}), "tpos=TRUE");
  EXPECT_EQ(FormatArgs("t", "tfal", htri_t{0}), "tfal=FALSE");
  EXPECT_EQ(FormatArgs("t", "tneg", htri_t{-2}), "tneg=FAIL(-2)");
  EXPECT_EQ(FormatArgs("Ul", "ul", 70000ul), "ul=70000");
  EXPECT_EQ(FormatArgs("UL", "ull", 9000000ull), "ull=9000000");
  EXPECT_THAT(FormatArgs("x", "p", &dummy), HasSubstr("p=0x"));
  EXPECT_EQ(FormatArgs("x", "np", nullptr), "np=NULL");
  EXPECT_EQ(FormatArgs("z", "z", static_cast<size_t>(128)), "z=128");
  EXPECT_EQ(FormatArgs("Zs", "zs", static_cast<ssize_t>(-64)), "zs=-64");
  EXPECT_EQ(FormatArgs("#!", "u", "v"),
            "u=Unsupported type slipped through!, v=Unknown type slipped "
            "through!");
}

TEST(H5traceTest, FormatsAttributeCacheAndDatasetCodes) {
  H5A_info_t ainfo;
  std::memset(&ainfo, 0, sizeof(ainfo));
  ainfo.corder_valid = true;
  ainfo.corder = 3;
  ainfo.cset = H5T_CSET_UTF8;
  ainfo.data_size = 64;
  EXPECT_EQ(FormatArgs("Ai", "ai", ainfo), "ai={TRUE, 3, H5T_CSET_UTF864}");
#ifndef H5_NO_DEPRECATED_SYMBOLS
  EXPECT_THAT(FormatArgs("Ao", "aop1", static_cast<H5A_operator1_t>(nullptr)),
              HasSubstr("aop1="));
#endif
  EXPECT_THAT(FormatArgs("AO", "aop2", static_cast<H5A_operator2_t>(nullptr)),
              HasSubstr("aop2="));

  H5AC_cache_config_t cc;
  std::memset(&cc, 0, sizeof(cc));
  cc.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  std::snprintf(cc.trace_file_name, sizeof(cc.trace_file_name), "mdc.trc");
  cc.rpt_fcn_enabled = true;
  cc.evictions_enabled = true;
  cc.incr_mode = H5C_incr__off;
  cc.flash_incr_mode = H5C_flash_incr__off;
  cc.decr_mode = H5C_decr__off;
  EXPECT_THAT(FormatArgs("Cc", "cc", cc), HasSubstr("'mdc.trc'"));

  cc.incr_mode = H5C_incr__threshold;
  cc.flash_incr_mode = H5C_flash_incr__add_space;
  cc.decr_mode = H5C_decr__threshold;
  EXPECT_THAT(FormatArgs("Cc", "cc", cc), HasSubstr("H5C_incr__threshold"));

  cc.decr_mode = H5C_decr__age_out;
  EXPECT_THAT(FormatArgs("Cc", "cc", cc), HasSubstr("H5C_decr__age_out"));

  cc.decr_mode = H5C_decr__age_out_with_threshold;
  EXPECT_THAT(FormatArgs("Cc", "cc", cc),
              HasSubstr("H5C_decr__age_out_with_threshold"));

  cc.incr_mode = static_cast<H5C_cache_incr_mode>(99);
  cc.flash_incr_mode = static_cast<H5C_cache_flash_incr_mode>(98);
  cc.decr_mode = static_cast<H5C_cache_decr_mode>(97);
  EXPECT_THAT(FormatArgs("Cc", "cc", cc), HasSubstr(", 99, "));

  H5AC_cache_image_config_t cic;
  std::memset(&cic, 0, sizeof(cic));
  cic.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
  cic.generate_image = true;
  cic.save_resize_status = false;
  cic.entry_ageout = 5;
  EXPECT_EQ(FormatArgs("CC", "cic", cic), "cic={1, TRUE, FALSE, 5}");

  // D* codes
  for (int v : {static_cast<int>(H5D_ALLOC_TIME_ERROR),
                static_cast<int>(H5D_ALLOC_TIME_DEFAULT),
                static_cast<int>(H5D_ALLOC_TIME_EARLY),
                static_cast<int>(H5D_ALLOC_TIME_LATE),
                static_cast<int>(H5D_ALLOC_TIME_INCR), 99}) {
    EXPECT_THAT(FormatArgs("Da", "v", v), HasSubstr("v="));
  }
  EXPECT_THAT(FormatArgs("DA", "cb", static_cast<H5D_append_cb_t>(nullptr)),
              HasSubstr("cb="));
  for (int v : {static_cast<int>(H5FD_MPIO_COLLECTIVE_IO),
                static_cast<int>(H5FD_MPIO_INDIVIDUAL_IO), 99}) {
    EXPECT_THAT(FormatArgs("Dc", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5D_SELECTION_IO_MODE_DEFAULT),
                static_cast<int>(H5D_SELECTION_IO_MODE_OFF),
                static_cast<int>(H5D_SELECTION_IO_MODE_ON), 99}) {
    EXPECT_THAT(FormatArgs("DC", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5D_FILL_TIME_ERROR),
                static_cast<int>(H5D_FILL_TIME_ALLOC),
                static_cast<int>(H5D_FILL_TIME_NEVER),
                static_cast<int>(H5D_FILL_TIME_IFSET), 99}) {
    EXPECT_THAT(FormatArgs("Df", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5D_FILL_VALUE_ERROR),
                static_cast<int>(H5D_FILL_VALUE_UNDEFINED),
                static_cast<int>(H5D_FILL_VALUE_DEFAULT),
                static_cast<int>(H5D_FILL_VALUE_USER_DEFINED), 99}) {
    EXPECT_THAT(FormatArgs("DF", "v", v), HasSubstr("v="));
  }
  EXPECT_THAT(FormatArgs("Dg", "cb", static_cast<H5D_gather_func_t>(nullptr)),
              HasSubstr("cb="));
  for (int v : {static_cast<int>(H5FD_MPIO_CHUNK_DEFAULT),
                static_cast<int>(H5FD_MPIO_CHUNK_ONE_IO),
                static_cast<int>(H5FD_MPIO_CHUNK_MULTI_IO), 99}) {
    EXPECT_THAT(FormatArgs("Dh", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5D_MPIO_NO_COLLECTIVE),
                static_cast<int>(H5D_MPIO_CHUNK_INDEPENDENT),
                static_cast<int>(H5D_MPIO_CHUNK_COLLECTIVE),
                static_cast<int>(H5D_MPIO_CHUNK_MIXED),
                static_cast<int>(H5D_MPIO_CONTIGUOUS_COLLECTIVE), 99}) {
    EXPECT_THAT(FormatArgs("Di", "v", v), HasSubstr("v="));
  }

  H5FD_file_image_callbacks_t ficb;
  std::memset(&ficb, 0, sizeof(ficb));
  EXPECT_THAT(FormatArgs("DI", "fi", ficb), HasSubstr("fi={"));

  for (int v : {static_cast<int>(H5D_CHUNK_IDX_BTREE),
                static_cast<int>(H5D_CHUNK_IDX_NONE),
                static_cast<int>(H5D_CHUNK_IDX_FARRAY),
                static_cast<int>(H5D_CHUNK_IDX_EARRAY),
                static_cast<int>(H5D_CHUNK_IDX_BT2),
                static_cast<int>(H5D_CHUNK_IDX_SINGLE),
                static_cast<int>(H5D_CHUNK_IDX_NTYPES), 99}) {
    EXPECT_THAT(FormatArgs("Dk", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5D_LAYOUT_ERROR),
                static_cast<int>(H5D_COMPACT),
                static_cast<int>(H5D_CONTIGUOUS),
                static_cast<int>(H5D_CHUNKED),
                static_cast<int>(H5D_VIRTUAL),
                static_cast<int>(H5D_NLAYOUTS), 99}) {
    EXPECT_THAT(FormatArgs("Dl", "v", v), HasSubstr("v="));
  }

  EXPECT_EQ(FormatArgs("Dn", "n", static_cast<int>(H5D_MPIO_COLLECTIVE)),
            "n=<none>");
  for (int bit : {H5D_MPIO_SET_INDEPENDENT, H5D_MPIO_DATATYPE_CONVERSION,
                  H5D_MPIO_DATA_TRANSFORMS,
                  H5D_MPIO_MPI_OPT_TYPES_ENV_VAR_DISABLED,
                  H5D_MPIO_NOT_SIMPLE_OR_SCALAR_DATASPACES,
                  H5D_MPIO_NOT_CONTIGUOUS_OR_CHUNKED_DATASET}) {
    EXPECT_THAT(FormatArgs("Dn", "n", bit), Not(HasSubstr(" | ")));
  }
  const int all_nocol = H5D_MPIO_SET_INDEPENDENT | H5D_MPIO_DATATYPE_CONVERSION |
                        H5D_MPIO_DATA_TRANSFORMS |
                        H5D_MPIO_MPI_OPT_TYPES_ENV_VAR_DISABLED |
                        H5D_MPIO_NOT_SIMPLE_OR_SCALAR_DATASPACES |
                        H5D_MPIO_NOT_CONTIGUOUS_OR_CHUNKED_DATASET;
  EXPECT_THAT(FormatArgs("Dn", "n", all_nocol), HasSubstr(" | "));

  for (int v : {static_cast<int>(H5D_MPIO_NO_CHUNK_OPTIMIZATION),
                static_cast<int>(H5D_MPIO_LINK_CHUNK),
                static_cast<int>(H5D_MPIO_MULTI_CHUNK), 99}) {
    EXPECT_THAT(FormatArgs("Do", "v", v), HasSubstr("v="));
  }
  EXPECT_THAT(FormatArgs("DO", "cb", static_cast<H5D_operator_t>(nullptr)),
              HasSubstr("cb="));
  for (int v : {static_cast<int>(H5D_SPACE_STATUS_NOT_ALLOCATED),
                static_cast<int>(H5D_SPACE_STATUS_PART_ALLOCATED),
                static_cast<int>(H5D_SPACE_STATUS_ALLOCATED),
                static_cast<int>(H5D_SPACE_STATUS_ERROR), 99}) {
    EXPECT_THAT(FormatArgs("Ds", "v", v), HasSubstr("v="));
  }
  EXPECT_THAT(FormatArgs("DS", "cb", static_cast<H5D_scatter_func_t>(nullptr)),
              HasSubstr("cb="));
  for (int v : {static_cast<int>(H5FD_MPIO_INDEPENDENT),
                static_cast<int>(H5FD_MPIO_COLLECTIVE), 99}) {
    EXPECT_THAT(FormatArgs("Dt", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5D_VDS_ERROR),
                static_cast<int>(H5D_VDS_FIRST_MISSING),
                static_cast<int>(H5D_VDS_LAST_AVAILABLE), 99}) {
    EXPECT_THAT(FormatArgs("Dv", "v", v), HasSubstr("v="));
  }
  for (H5FD_class_value_t v : {H5_VFD_INVALID, H5_VFD_SEC2, H5_VFD_CORE,
                               H5_VFD_LOG, H5_VFD_FAMILY, H5_VFD_MULTI,
                               H5_VFD_STDIO, H5_VFD_ONION, H5_VFD_SPLITTER,
                               static_cast<H5FD_class_value_t>(999)}) {
    EXPECT_THAT(FormatArgs("DV", "v", v), HasSubstr("v="));
  }
}

TEST(H5traceTest, FormatsErrorFileAndGroupCodes) {
  // E* codes
#ifndef H5_NO_DEPRECATED_SYMBOLS
  EXPECT_THAT(FormatArgs("Ea", "cb", static_cast<H5E_auto1_t>(nullptr)),
              HasSubstr("cb="));
#endif
  EXPECT_THAT(FormatArgs("EA", "cb", static_cast<H5E_auto2_t>(nullptr)),
              HasSubstr("cb="));
  EXPECT_THAT(
      FormatArgs("EC", "cb", static_cast<H5ES_event_complete_func_t>(nullptr)),
      HasSubstr("cb="));
  for (int v : {static_cast<int>(H5E_WALK_UPWARD),
                static_cast<int>(H5E_WALK_DOWNWARD), 99}) {
    EXPECT_THAT(FormatArgs("Ed", "v", v), HasSubstr("v="));
  }
  H5E_error2_t err2;
  std::memset(&err2, 0, sizeof(err2));
  EXPECT_THAT(FormatArgs("Ee", "e", &err2), HasSubstr("e="));
  EXPECT_THAT(
      FormatArgs("EI", "cb", static_cast<H5ES_event_insert_func_t>(nullptr)),
      HasSubstr("cb="));
  for (int v : {static_cast<int>(H5ES_STATUS_IN_PROGRESS),
                static_cast<int>(H5ES_STATUS_SUCCEED),
                static_cast<int>(H5ES_STATUS_CANCELED),
                static_cast<int>(H5ES_STATUS_FAIL), 99}) {
    EXPECT_THAT(FormatArgs("Es", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5E_MAJOR), static_cast<int>(H5E_MINOR), 99}) {
    EXPECT_THAT(FormatArgs("Et", "v", v), HasSubstr("v="));
  }

  // F* codes
  H5FD_class_t fcls;
  std::memset(&fcls, 0, sizeof(fcls));
  fcls.name = "core";
  fcls.maxaddr = 4096;
  fcls.fc_degree = H5F_CLOSE_SEMI;
  EXPECT_THAT(FormatArgs("FC", "fc", fcls),
              HasSubstr("fc={'core', 4096, H5F_CLOSE_SEMI, ...}"));

  for (int v : {static_cast<int>(H5F_CLOSE_DEFAULT),
                static_cast<int>(H5F_CLOSE_WEAK),
                static_cast<int>(H5F_CLOSE_SEMI),
                static_cast<int>(H5F_CLOSE_STRONG), 99}) {
    EXPECT_THAT(FormatArgs("Fd", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5F_FSPACE_STRATEGY_FSM_AGGR),
                static_cast<int>(H5F_FSPACE_STRATEGY_PAGE),
                static_cast<int>(H5F_FSPACE_STRATEGY_AGGR),
                static_cast<int>(H5F_FSPACE_STRATEGY_NONE),
                static_cast<int>(H5F_FSPACE_STRATEGY_NTYPES), 99}) {
    EXPECT_THAT(FormatArgs("Ff", "v", v), HasSubstr("v="));
  }
  EXPECT_THAT(FormatArgs("FF", "cb", static_cast<H5F_flush_cb_t>(nullptr)),
              HasSubstr("cb="));

  H5F_info2_t fi2;
  std::memset(&fi2, 0, sizeof(fi2));
  fi2.super.version = 3;
  fi2.super.super_size = 512;
  fi2.super.super_ext_size = 64;
  fi2.free.version = 1;
  fi2.free.meta_size = 128;
  fi2.free.tot_space = 256;
  fi2.sohm.version = 0;
  fi2.sohm.hdr_size = 32;
  fi2.sohm.msgs_info.index_size = 16;
  fi2.sohm.msgs_info.heap_size = 8;
  EXPECT_EQ(FormatArgs("FI", "fi", fi2),
            "fi={{3, 512, 64}, {1, 128, 256}, {0, 32, {16, 8}}}");

  for (int v : {static_cast<int>(H5FD_MEM_NOLIST),
                static_cast<int>(H5FD_MEM_DEFAULT),
                static_cast<int>(H5FD_MEM_SUPER),
                static_cast<int>(H5FD_MEM_BTREE),
                static_cast<int>(H5FD_MEM_DRAW),
                static_cast<int>(H5FD_MEM_GHEAP),
                static_cast<int>(H5FD_MEM_LHEAP),
                static_cast<int>(H5FD_MEM_OHDR),
                static_cast<int>(H5FD_MEM_NTYPES), 99}) {
    EXPECT_THAT(FormatArgs("Fm", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5F_SCOPE_LOCAL),
                static_cast<int>(H5F_SCOPE_GLOBAL), 99}) {
    EXPECT_THAT(FormatArgs("Fs", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5F_FILE_SPACE_DEFAULT),
                static_cast<int>(H5F_FILE_SPACE_ALL_PERSIST),
                static_cast<int>(H5F_FILE_SPACE_ALL),
                static_cast<int>(H5F_FILE_SPACE_AGGR_VFD),
                static_cast<int>(H5F_FILE_SPACE_VFD),
                static_cast<int>(H5F_FILE_SPACE_NTYPES), 99}) {
    EXPECT_THAT(FormatArgs("Ft", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5F_LIBVER_EARLIEST),
                static_cast<int>(H5F_LIBVER_V18),
                static_cast<int>(H5F_LIBVER_V110),
                static_cast<int>(H5F_LIBVER_V112),
                static_cast<int>(H5F_LIBVER_V114),
                static_cast<int>(H5F_LIBVER_V200),
                static_cast<int>(H5F_LIBVER_ERROR),
                static_cast<int>(H5F_LIBVER_NBOUNDS), 99}) {
    EXPECT_THAT(FormatArgs("Fv", "v", v), HasSubstr("v="));
  }

  // G* codes
#ifndef H5_NO_DEPRECATED_SYMBOLS
  EXPECT_THAT(FormatArgs("Gi", "cb", static_cast<H5G_iterate_t>(nullptr)),
              HasSubstr("cb="));
  for (int st : {static_cast<int>(H5G_STORAGE_TYPE_UNKNOWN),
                 static_cast<int>(H5G_STORAGE_TYPE_SYMBOL_TABLE),
                 static_cast<int>(H5G_STORAGE_TYPE_COMPACT),
                 static_cast<int>(H5G_STORAGE_TYPE_DENSE), 99}) {
    H5G_info_t ginfo;
    std::memset(&ginfo, 0, sizeof(ginfo));
    ginfo.storage_type = static_cast<H5G_storage_type_t>(st);
    ginfo.nlinks = 2;
    ginfo.max_corder = 1;
    ginfo.mounted = (st == H5G_STORAGE_TYPE_DENSE);
    EXPECT_THAT(FormatArgs("GI", "gi", ginfo), HasSubstr("gi={"));
  }
  for (int v : {static_cast<int>(H5G_UNKNOWN), static_cast<int>(H5G_GROUP),
                static_cast<int>(H5G_DATASET), static_cast<int>(H5G_TYPE),
                static_cast<int>(H5G_LINK), static_cast<int>(H5G_UDLINK),
                static_cast<int>(H5G_RESERVED_5),
                static_cast<int>(H5G_RESERVED_6),
                static_cast<int>(H5G_RESERVED_7), 99}) {
    EXPECT_THAT(FormatArgs("Go", "v", v), HasSubstr("v="));
  }
  EXPECT_THAT(FormatArgs("Gs", "s", static_cast<H5G_stat_t*>(nullptr)),
              HasSubstr("s="));
#endif
  EXPECT_THAT(FormatArgs("Hc", "cb", static_cast<H5_atclose_func_t>(nullptr)),
              HasSubstr("cb="));
}

TEST(H5traceTest, FormatsIdsAndAllPredefinedDatatypes) {
  ASSERT_GE(H5open(), 0);

  EXPECT_EQ(FormatArgs("i", "def", H5P_DEFAULT), "def=H5P_DEFAULT");
  EXPECT_EQ(FormatArgs("i", "bad", static_cast<hid_t>(-1)), "bad=FAIL");

  // All predefined datatypes in H5trace.c
  const std::vector<std::pair<hid_t, const char*>> dtypes = {
      {H5T_NATIVE_SCHAR, "H5T_NATIVE_SCHAR"},
      {H5T_NATIVE_UCHAR, "H5T_NATIVE_UCHAR"},
      {H5T_NATIVE_SHORT, "H5T_NATIVE_SHORT"},
      {H5T_NATIVE_USHORT, "H5T_NATIVE_USHORT"},
      {H5T_NATIVE_INT, "H5T_NATIVE_INT"},
      {H5T_NATIVE_UINT, "H5T_NATIVE_UINT"},
      {H5T_NATIVE_LONG, "H5T_NATIVE_LONG"},
      {H5T_NATIVE_ULONG, "H5T_NATIVE_ULONG"},
      {H5T_NATIVE_LLONG, "H5T_NATIVE_LLONG"},
      {H5T_NATIVE_ULLONG, "H5T_NATIVE_ULLONG"},
#ifdef H5_HAVE__FLOAT16
      {H5T_NATIVE_FLOAT16, "H5T_NATIVE_FLOAT16"},
#endif
      {H5T_NATIVE_FLOAT, "H5T_NATIVE_FLOAT"},
      {H5T_NATIVE_DOUBLE, "H5T_NATIVE_DOUBLE"},
      {H5T_NATIVE_LDOUBLE, "H5T_NATIVE_LDOUBLE"},
#ifdef H5_HAVE_COMPLEX_NUMBERS
      {H5T_NATIVE_FLOAT_COMPLEX, "H5T_NATIVE_FLOAT_COMPLEX"},
      {H5T_NATIVE_DOUBLE_COMPLEX, "H5T_NATIVE_DOUBLE_COMPLEX"},
      {H5T_NATIVE_LDOUBLE_COMPLEX, "H5T_NATIVE_LDOUBLE_COMPLEX"},
#endif
      {H5T_IEEE_F16BE, "H5T_IEEE_F16BE"},
      {H5T_IEEE_F16LE, "H5T_IEEE_F16LE"},
      {H5T_IEEE_F32BE, "H5T_IEEE_F32BE"},
      {H5T_IEEE_F32LE, "H5T_IEEE_F32LE"},
      {H5T_IEEE_F64BE, "H5T_IEEE_F64BE"},
      {H5T_IEEE_F64LE, "H5T_IEEE_F64LE"},
      {H5T_FLOAT_BFLOAT16BE, "H5T_FLOAT_BFLOAT16BE"},
      {H5T_FLOAT_BFLOAT16LE, "H5T_FLOAT_BFLOAT16LE"},
      {H5T_FLOAT_F8E4M3, "H5T_FLOAT_F8E4M3"},
      {H5T_FLOAT_F8E5M2, "H5T_FLOAT_F8E5M2"},
      {H5T_FLOAT_F6E2M3, "H5T_FLOAT_F6E2M3"},
      {H5T_FLOAT_F6E3M2, "H5T_FLOAT_F6E3M2"},
      {H5T_FLOAT_F4E2M1, "H5T_FLOAT_F4E2M1"},
      {H5T_COMPLEX_IEEE_F16BE, "H5T_COMPLEX_IEEE_F16BE"},
      {H5T_COMPLEX_IEEE_F16LE, "H5T_COMPLEX_IEEE_F16LE"},
      {H5T_COMPLEX_IEEE_F32BE, "H5T_COMPLEX_IEEE_F32BE"},
      {H5T_COMPLEX_IEEE_F32LE, "H5T_COMPLEX_IEEE_F32LE"},
      {H5T_COMPLEX_IEEE_F64BE, "H5T_COMPLEX_IEEE_F64BE"},
      {H5T_COMPLEX_IEEE_F64LE, "H5T_COMPLEX_IEEE_F64LE"},
      {H5T_STD_I8BE, "H5T_STD_I8BE"},
      {H5T_STD_I8LE, "H5T_STD_I8LE"},
      {H5T_STD_I16BE, "H5T_STD_I16BE"},
      {H5T_STD_I16LE, "H5T_STD_I16LE"},
      {H5T_STD_I32BE, "H5T_STD_I32BE"},
      {H5T_STD_I32LE, "H5T_STD_I32LE"},
      {H5T_STD_I64BE, "H5T_STD_I64BE"},
      {H5T_STD_I64LE, "H5T_STD_I64LE"},
      {H5T_STD_U8BE, "H5T_STD_U8BE"},
      {H5T_STD_U8LE, "H5T_STD_U8LE"},
      {H5T_STD_U16BE, "H5T_STD_U16BE"},
      {H5T_STD_U16LE, "H5T_STD_U16LE"},
      {H5T_STD_U32BE, "H5T_STD_U32BE"},
      {H5T_STD_U32LE, "H5T_STD_U32LE"},
      {H5T_STD_U64BE, "H5T_STD_U64BE"},
      {H5T_STD_U64LE, "H5T_STD_U64LE"},
      {H5T_STD_B8BE, "H5T_STD_B8BE"},
      {H5T_STD_B8LE, "H5T_STD_B8LE"},
      {H5T_STD_B16BE, "H5T_STD_B16BE"},
      {H5T_STD_B16LE, "H5T_STD_B16LE"},
      {H5T_STD_B32BE, "H5T_STD_B32BE"},
      {H5T_STD_B32LE, "H5T_STD_B32LE"},
      {H5T_STD_B64BE, "H5T_STD_B64BE"},
      {H5T_STD_B64LE, "H5T_STD_B64LE"},
      {H5T_C_S1, "H5T_C_S1"},
      {H5T_FORTRAN_S1, "H5T_FORTRAN_S1"},
  };
  for (const auto& [tid, name] : dtypes) {
    EXPECT_EQ(FormatArgs("i", "dt", tid), std::string("dt=") + name);
  }

  ScopedType custom_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(custom_dt.valid());
  EXPECT_THAT(FormatArgs("i", "dt", custom_dt.get()), HasSubstr("(dtype)"));

  // Dataspace IDs: simple (populates asize), scalar, and unmapped synthetic ID.
  hsize_t dims[2] = {3, 4};
  ScopedSpace simple_sp(H5Screate_simple(2, dims, nullptr));
  ASSERT_TRUE(simple_sp.valid());
  EXPECT_THAT(FormatArgs("i*[a0]h", "sp", simple_sp.get(), "dims", dims),
              HasSubstr("(dspace), dims="));
  EXPECT_THAT(FormatArgs("i*[a0]h", "sp", simple_sp.get(), "dims", dims),
              HasSubstr("{3, 4}"));

  ScopedSpace scalar_sp(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar_sp.valid());
  EXPECT_THAT(FormatArgs("i*[a0]h", "sp", scalar_sp.get(), "dims", dims),
              Not(HasSubstr("{")));

  const hid_t missing_sp = MakeSyntheticId(H5I_DATASPACE, 0x7ffff);
  EXPECT_THAT(FormatArgs("i", "sp", missing_sp), HasSubstr("(dspace)"));

  // All remaining H5I classes
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_FILE, 1)),
              HasSubstr("(file)"));
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_GROUP, 1)),
              HasSubstr("(group)"));
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_DATASET, 1)),
              HasSubstr("(dset)"));
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_ATTR, 1)),
              HasSubstr("(attr)"));
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_MAP, 1)),
              HasSubstr("(map)"));
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_VFL, 1)),
              HasSubstr("(file driver)"));
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_VOL, 1)),
              HasSubstr("(VOL plugin)"));
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_GENPROP_CLS, 1)),
              HasSubstr("(genprop class)"));
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_GENPROP_LST, 1)),
              HasSubstr("(genprop list)"));
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_ERROR_CLASS, 1)),
              HasSubstr("(err class)"));
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_ERROR_MSG, 1)),
              HasSubstr("(err msg)"));
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_ERROR_STACK, 1)),
              HasSubstr("(err stack)"));
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_SPACE_SEL_ITER, 1)),
              HasSubstr("(dataspace selection iterator)"));
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_EVENTSET, 1)),
              HasSubstr("(event set)"));
  EXPECT_THAT(FormatArgs("i", "v", MakeSyntheticId(H5I_NTYPES, 1)),
              HasSubstr("(ntypes - error)"));
  EXPECT_THAT(FormatArgs("i", "v", static_cast<hid_t>(1)),
              HasSubstr("(unknown class)"));

  // H5P_class_t ('p')
  EXPECT_EQ(FormatArgs("p", "cls", H5P_FILE_ACCESS), "cls=file access");
  EXPECT_EQ(FormatArgs("p", "cls", static_cast<hid_t>(123)), "cls=123");
}

TEST(H5traceTest, FormatsRemainingModuleCodes) {
  ASSERT_GE(H5open(), 0);

  // I* codes
  EXPECT_THAT(
      FormatArgs("ID", "cb", static_cast<H5I_future_discard_func_t>(nullptr)),
      HasSubstr("cb="));
  EXPECT_THAT(FormatArgs("If", "cb", static_cast<H5I_free_t>(nullptr)),
              HasSubstr("cb="));
  for (int v : {static_cast<int>(H5_INDEX_UNKNOWN),
                static_cast<int>(H5_INDEX_NAME),
                static_cast<int>(H5_INDEX_CRT_ORDER),
                static_cast<int>(H5_INDEX_N), 99}) {
    EXPECT_THAT(FormatArgs("Ii", "v", v), HasSubstr("v="));
  }
  EXPECT_THAT(FormatArgs("II", "cb", static_cast<H5I_iterate_func_t>(nullptr)),
              HasSubstr("cb="));
  for (int v : {static_cast<int>(H5_ITER_UNKNOWN),
                static_cast<int>(H5_ITER_INC), static_cast<int>(H5_ITER_DEC),
                static_cast<int>(H5_ITER_NATIVE),
                static_cast<int>(H5_ITER_N), 99}) {
    EXPECT_THAT(FormatArgs("Io", "v", v), HasSubstr("v="));
  }
  EXPECT_THAT(
      FormatArgs("IR", "cb", static_cast<H5I_future_realize_func_t>(nullptr)),
      HasSubstr("cb="));
  EXPECT_THAT(FormatArgs("IS", "cb", static_cast<H5I_search_func_t>(nullptr)),
              HasSubstr("cb="));
  for (int v : {static_cast<int>(H5I_UNINIT), static_cast<int>(H5I_BADID),
                static_cast<int>(H5I_FILE), static_cast<int>(H5I_GROUP),
                static_cast<int>(H5I_DATATYPE), static_cast<int>(H5I_DATASPACE),
                static_cast<int>(H5I_DATASET), static_cast<int>(H5I_ATTR),
                static_cast<int>(H5I_MAP), static_cast<int>(H5I_VFL),
                static_cast<int>(H5I_VOL), static_cast<int>(H5I_GENPROP_CLS),
                static_cast<int>(H5I_GENPROP_LST),
                static_cast<int>(H5I_ERROR_CLASS),
                static_cast<int>(H5I_ERROR_MSG),
                static_cast<int>(H5I_ERROR_STACK),
                static_cast<int>(H5I_SPACE_SEL_ITER),
                static_cast<int>(H5I_EVENTSET), static_cast<int>(H5I_NTYPES),
                99}) {
    EXPECT_THAT(FormatArgs("It", "v", v), HasSubstr("v="));
  }

  // k (H5O_token_t)
  H5O_token_t tok;
  std::memset(&tok, 0, sizeof(tok));
  tok.__data[0] = 0xab;
  tok.__data[1] = 0xcd;
  EXPECT_THAT(FormatArgs("k", "tok", tok), HasSubstr("tok=abcd"));

  // L* codes
#ifndef H5_NO_DEPRECATED_SYMBOLS
  EXPECT_THAT(FormatArgs("Li", "cb", static_cast<H5L_iterate1_t>(nullptr)),
              HasSubstr("cb="));
#endif
  EXPECT_THAT(FormatArgs("LI", "cb", static_cast<H5L_iterate2_t>(nullptr)),
              HasSubstr("cb="));
  for (int v : {static_cast<int>(H5L_TYPE_ERROR),
                static_cast<int>(H5L_TYPE_HARD),
                static_cast<int>(H5L_TYPE_SOFT),
                static_cast<int>(H5L_TYPE_EXTERNAL),
                static_cast<int>(H5L_TYPE_MAX), 99}) {
    EXPECT_THAT(FormatArgs("Ll", "v", v), HasSubstr("v="));
  }
  EXPECT_THAT(FormatArgs("Lt", "cb", static_cast<H5L_elink_traverse_t>(nullptr)),
              HasSubstr("cb="));

  // M* codes
  EXPECT_THAT(FormatArgs("Ma", "cb", static_cast<H5MM_allocate_t>(nullptr)),
              HasSubstr("cb="));
  EXPECT_THAT(FormatArgs("Mf", "cb", static_cast<H5MM_free_t>(nullptr)),
              HasSubstr("cb="));
  for (int v : {static_cast<int>(H5FD_MEM_NOLIST),
                static_cast<int>(H5FD_MEM_DEFAULT),
                static_cast<int>(H5FD_MEM_SUPER),
                static_cast<int>(H5FD_MEM_BTREE),
                static_cast<int>(H5FD_MEM_DRAW),
                static_cast<int>(H5FD_MEM_GHEAP),
                static_cast<int>(H5FD_MEM_LHEAP),
                static_cast<int>(H5FD_MEM_OHDR),
                static_cast<int>(H5FD_MEM_NTYPES), 99}) {
    EXPECT_THAT(FormatArgs("Mt", "v", v), HasSubstr("v="));
  }

  // O* codes
#ifndef H5_NO_DEPRECATED_SYMBOLS
  EXPECT_THAT(FormatArgs("Oi", "cb", static_cast<H5O_iterate1_t>(nullptr)),
              HasSubstr("cb="));
#endif
  EXPECT_THAT(FormatArgs("OI", "cb", static_cast<H5O_iterate2_t>(nullptr)),
              HasSubstr("cb="));
  EXPECT_THAT(
      FormatArgs("Os", "cb", static_cast<H5O_mcdt_search_cb_t>(nullptr)),
      HasSubstr("cb="));
  for (int v : {static_cast<int>(H5O_TYPE_UNKNOWN),
                static_cast<int>(H5O_TYPE_GROUP),
                static_cast<int>(H5O_TYPE_DATASET),
                static_cast<int>(H5O_TYPE_NAMED_DATATYPE),
                static_cast<int>(H5O_TYPE_MAP),
                static_cast<int>(H5O_TYPE_NTYPES), 99}) {
    EXPECT_THAT(FormatArgs("Ot", "v", v), HasSubstr("v="));
  }

  // P* callbacks
  EXPECT_THAT(
      FormatArgs("Pc", "cb", static_cast<H5P_cls_create_func_t>(nullptr)),
      HasSubstr("cb="));
  EXPECT_THAT(
      FormatArgs("PC", "cb", static_cast<H5P_prp_create_func_t>(nullptr)),
      HasSubstr("cb="));
  EXPECT_THAT(
      FormatArgs("PD", "cb", static_cast<H5P_prp_delete_func_t>(nullptr)),
      HasSubstr("cb="));
  EXPECT_THAT(FormatArgs("PG", "cb", static_cast<H5P_prp_get_func_t>(nullptr)),
              HasSubstr("cb="));
  EXPECT_THAT(FormatArgs("Pi", "cb", static_cast<H5P_iterate_t>(nullptr)),
              HasSubstr("cb="));
  EXPECT_THAT(
      FormatArgs("Pl", "cb", static_cast<H5P_cls_close_func_t>(nullptr)),
      HasSubstr("cb="));
  EXPECT_THAT(
      FormatArgs("PL", "cb", static_cast<H5P_prp_close_func_t>(nullptr)),
      HasSubstr("cb="));
  EXPECT_THAT(
      FormatArgs("PM", "cb", static_cast<H5P_prp_compare_func_t>(nullptr)),
      HasSubstr("cb="));
  EXPECT_THAT(FormatArgs("Po", "cb", static_cast<H5P_cls_copy_func_t>(nullptr)),
              HasSubstr("cb="));
  EXPECT_THAT(FormatArgs("PO", "cb", static_cast<H5P_prp_copy_func_t>(nullptr)),
              HasSubstr("cb="));
  EXPECT_THAT(FormatArgs("PS", "cb", static_cast<H5P_prp_set_func_t>(nullptr)),
              HasSubstr("cb="));

  // R* valid codes
  EXPECT_EQ(FormatArgs("Ro", "ref", static_cast<hobj_ref_t>(55)),
            "ref=Reference Object=55");
  for (int v : {static_cast<int>(H5R_BADTYPE), static_cast<int>(H5R_OBJECT1),
                static_cast<int>(H5R_DATASET_REGION1),
                static_cast<int>(H5R_OBJECT2),
                static_cast<int>(H5R_DATASET_REGION2),
                static_cast<int>(H5R_ATTR), static_cast<int>(H5R_MAXTYPE),
                99}) {
    EXPECT_THAT(FormatArgs("Rt", "v", v), HasSubstr("v="));
  }

  // S* codes
  for (int v : {static_cast<int>(H5S_NO_CLASS), static_cast<int>(H5S_SCALAR),
                static_cast<int>(H5S_SIMPLE), static_cast<int>(H5S_NULL), 99}) {
    EXPECT_THAT(FormatArgs("Sc", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5S_SELECT_NOOP),
                static_cast<int>(H5S_SELECT_SET),
                static_cast<int>(H5S_SELECT_OR),
                static_cast<int>(H5S_SELECT_AND),
                static_cast<int>(H5S_SELECT_XOR),
                static_cast<int>(H5S_SELECT_NOTB),
                static_cast<int>(H5S_SELECT_NOTA),
                static_cast<int>(H5S_SELECT_APPEND),
                static_cast<int>(H5S_SELECT_PREPEND),
                static_cast<int>(H5S_SELECT_INVALID), 99}) {
    EXPECT_THAT(FormatArgs("Ss", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5S_SEL_ERROR), static_cast<int>(H5S_SEL_NONE),
                static_cast<int>(H5S_SEL_POINTS),
                static_cast<int>(H5S_SEL_HYPERSLABS),
                static_cast<int>(H5S_SEL_ALL), static_cast<int>(H5S_SEL_N),
                99}) {
    EXPECT_THAT(FormatArgs("St", "v", v), HasSubstr("v="));
  }

  // T* codes
  for (int v : {static_cast<int>(H5T_CSET_ERROR),
                static_cast<int>(H5T_CSET_ASCII),
                static_cast<int>(H5T_CSET_UTF8),
                static_cast<int>(H5T_CSET_RESERVED_2),
                static_cast<int>(H5T_CSET_RESERVED_15), 99}) {
    EXPECT_THAT(FormatArgs("Tc", "v", v), HasSubstr("v="));
  }
  EXPECT_THAT(FormatArgs("TC", "cb", static_cast<H5T_conv_t>(nullptr)),
              HasSubstr("cb="));
  for (int v : {static_cast<int>(H5T_DIR_DEFAULT),
                static_cast<int>(H5T_DIR_ASCEND),
                static_cast<int>(H5T_DIR_DESCEND), 99}) {
    EXPECT_THAT(FormatArgs("Td", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5T_PERS_DONTCARE),
                static_cast<int>(H5T_PERS_SOFT),
                static_cast<int>(H5T_PERS_HARD), 99}) {
    EXPECT_THAT(FormatArgs("Te", "v", v), HasSubstr("v="));
  }
  EXPECT_THAT(
      FormatArgs("TE", "cb", static_cast<H5T_conv_except_func_t>(nullptr)),
      HasSubstr("cb="));
  for (int v : {static_cast<int>(H5T_NORM_ERROR),
                static_cast<int>(H5T_NORM_IMPLIED),
                static_cast<int>(H5T_NORM_MSBSET),
                static_cast<int>(H5T_NORM_NONE), 99}) {
    EXPECT_THAT(FormatArgs("Tn", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5T_ORDER_ERROR),
                static_cast<int>(H5T_ORDER_LE), static_cast<int>(H5T_ORDER_BE),
                static_cast<int>(H5T_ORDER_VAX),
                static_cast<int>(H5T_ORDER_MIXED),
                static_cast<int>(H5T_ORDER_NONE), 99}) {
    EXPECT_THAT(FormatArgs("To", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5T_PAD_ERROR), static_cast<int>(H5T_PAD_ZERO),
                static_cast<int>(H5T_PAD_ONE),
                static_cast<int>(H5T_PAD_BACKGROUND),
                static_cast<int>(H5T_NPAD), 99}) {
    EXPECT_THAT(FormatArgs("Tp", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5T_SGN_ERROR), static_cast<int>(H5T_SGN_NONE),
                static_cast<int>(H5T_SGN_2), static_cast<int>(H5T_NSGN), 99}) {
    EXPECT_THAT(FormatArgs("Ts", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5T_NO_CLASS), static_cast<int>(H5T_INTEGER),
                static_cast<int>(H5T_FLOAT), static_cast<int>(H5T_TIME),
                static_cast<int>(H5T_STRING), static_cast<int>(H5T_BITFIELD),
                static_cast<int>(H5T_OPAQUE), static_cast<int>(H5T_COMPOUND),
                static_cast<int>(H5T_REFERENCE), static_cast<int>(H5T_ENUM),
                static_cast<int>(H5T_VLEN), static_cast<int>(H5T_ARRAY),
                static_cast<int>(H5T_COMPLEX), static_cast<int>(H5T_NCLASSES),
                99}) {
    EXPECT_THAT(FormatArgs("Tt", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5T_STR_ERROR),
                static_cast<int>(H5T_STR_NULLTERM),
                static_cast<int>(H5T_STR_NULLPAD),
                static_cast<int>(H5T_STR_SPACEPAD),
                static_cast<int>(H5T_STR_RESERVED_3),
                static_cast<int>(H5T_STR_RESERVED_15), 99}) {
    EXPECT_THAT(FormatArgs("Tz", "v", v), HasSubstr("v="));
  }

  // Z* codes
  for (int v : {static_cast<int>(H5Z_SO_FLOAT_DSCALE),
                static_cast<int>(H5Z_SO_FLOAT_ESCALE),
                static_cast<int>(H5Z_SO_INT), 99}) {
    EXPECT_THAT(FormatArgs("Za", "v", v), HasSubstr("v="));
  }
  EXPECT_THAT(FormatArgs("Zc", "f", static_cast<H5Z_class2_t*>(nullptr)),
              HasSubstr("f="));
  for (int v : {static_cast<int>(H5Z_DISABLE_EDC),
                static_cast<int>(H5Z_ENABLE_EDC), 99}) {
    EXPECT_THAT(FormatArgs("Ze", "v", v), HasSubstr("v="));
  }
  for (H5Z_filter_t v : {H5Z_FILTER_NONE, H5Z_FILTER_DEFLATE,
                         H5Z_FILTER_SHUFFLE, H5Z_FILTER_FLETCHER32,
                         H5Z_FILTER_SZIP, H5Z_FILTER_NBIT,
                         H5Z_FILTER_SCALEOFFSET, 999}) {
    EXPECT_THAT(FormatArgs("Zf", "v", v), HasSubstr("v="));
  }
  EXPECT_THAT(FormatArgs("ZF", "cb", static_cast<H5Z_filter_func_t>(nullptr)),
              HasSubstr("cb="));
}

TEST(H5traceTest, FormatsVolCodes) {
  for (int v : {static_cast<int>(H5VL_ATTR_GET_SPACE),
                static_cast<int>(H5VL_ATTR_GET_TYPE),
                static_cast<int>(H5VL_ATTR_GET_ACPL),
                static_cast<int>(H5VL_ATTR_GET_NAME),
                static_cast<int>(H5VL_ATTR_GET_STORAGE_SIZE),
                static_cast<int>(H5VL_ATTR_GET_INFO), 99}) {
    EXPECT_THAT(FormatArgs("Va", "v", v), HasSubstr("v="));
  }
  EXPECT_EQ(FormatArgs("VA", "v", 7), "v=7");
  for (int v : {static_cast<int>(H5VL_ATTR_DELETE),
                static_cast<int>(H5VL_ATTR_DELETE_BY_IDX),
                static_cast<int>(H5VL_ATTR_EXISTS),
                static_cast<int>(H5VL_ATTR_ITER),
                static_cast<int>(H5VL_ATTR_RENAME), 99}) {
    EXPECT_THAT(FormatArgs("Vb", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_BLOB_DELETE),
                static_cast<int>(H5VL_BLOB_ISNULL),
                static_cast<int>(H5VL_BLOB_SETNULL), 99}) {
    EXPECT_THAT(FormatArgs("VB", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_DATASET_GET_SPACE),
                static_cast<int>(H5VL_DATASET_GET_SPACE_STATUS),
                static_cast<int>(H5VL_DATASET_GET_TYPE),
                static_cast<int>(H5VL_DATASET_GET_DCPL),
                static_cast<int>(H5VL_DATASET_GET_DAPL),
                static_cast<int>(H5VL_DATASET_GET_STORAGE_SIZE), 99}) {
    EXPECT_THAT(FormatArgs("Vc", "v", v), HasSubstr("v="));
  }
  EXPECT_EQ(FormatArgs("VC", "v", static_cast<H5VL_class_value_t>(H5_VOL_NATIVE)),
            "v=H5_VOL_NATIVE");
  EXPECT_EQ(FormatArgs("VC", "v", static_cast<H5VL_class_value_t>(42)), "v=42");
  for (int v : {static_cast<int>(H5VL_DATASET_SET_EXTENT),
                static_cast<int>(H5VL_DATASET_FLUSH),
                static_cast<int>(H5VL_DATASET_REFRESH), 99}) {
    EXPECT_THAT(FormatArgs("Vd", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_DATATYPE_GET_BINARY_SIZE),
                static_cast<int>(H5VL_DATATYPE_GET_BINARY),
                static_cast<int>(H5VL_DATATYPE_GET_TCPL), 99}) {
    EXPECT_THAT(FormatArgs("Ve", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_DATATYPE_FLUSH),
                static_cast<int>(H5VL_DATATYPE_REFRESH), 99}) {
    EXPECT_THAT(FormatArgs("Vf", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_FILE_GET_CONT_INFO),
                static_cast<int>(H5VL_FILE_GET_FAPL),
                static_cast<int>(H5VL_FILE_GET_FCPL),
                static_cast<int>(H5VL_FILE_GET_FILENO),
                static_cast<int>(H5VL_FILE_GET_INTENT),
                static_cast<int>(H5VL_FILE_GET_NAME),
                static_cast<int>(H5VL_FILE_GET_OBJ_COUNT),
                static_cast<int>(H5VL_FILE_GET_OBJ_IDS), 99}) {
    EXPECT_THAT(FormatArgs("Vg", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_FILE_FLUSH),
                static_cast<int>(H5VL_FILE_REOPEN),
                static_cast<int>(H5VL_FILE_IS_ACCESSIBLE),
                static_cast<int>(H5VL_FILE_DELETE),
                static_cast<int>(H5VL_FILE_IS_EQUAL), 99}) {
    EXPECT_THAT(FormatArgs("Vh", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_GROUP_GET_GCPL),
                static_cast<int>(H5VL_GROUP_GET_INFO), 99}) {
    EXPECT_THAT(FormatArgs("Vi", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_GROUP_MOUNT),
                static_cast<int>(H5VL_GROUP_UNMOUNT),
                static_cast<int>(H5VL_GROUP_FLUSH),
                static_cast<int>(H5VL_GROUP_REFRESH), 99}) {
    EXPECT_THAT(FormatArgs("Vj", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_LINK_CREATE_HARD),
                static_cast<int>(H5VL_LINK_CREATE_SOFT),
                static_cast<int>(H5VL_LINK_CREATE_UD), 99}) {
    EXPECT_THAT(FormatArgs("Vk", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_LINK_GET_INFO),
                static_cast<int>(H5VL_LINK_GET_NAME),
                static_cast<int>(H5VL_LINK_GET_VAL), 99}) {
    EXPECT_THAT(FormatArgs("Vl", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_GET_CONN_LVL_CURR),
                static_cast<int>(H5VL_GET_CONN_LVL_TERM), 99}) {
    EXPECT_THAT(FormatArgs("VL", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_LINK_DELETE),
                static_cast<int>(H5VL_LINK_EXISTS),
                static_cast<int>(H5VL_LINK_ITER), 99}) {
    EXPECT_THAT(FormatArgs("Vm", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_OBJECT_GET_FILE),
                static_cast<int>(H5VL_OBJECT_GET_NAME),
                static_cast<int>(H5VL_OBJECT_GET_TYPE),
                static_cast<int>(H5VL_OBJECT_GET_INFO), 99}) {
    EXPECT_THAT(FormatArgs("Vn", "v", v), HasSubstr("v="));
  }
  EXPECT_THAT(
      FormatArgs("VN", "cb", static_cast<H5VL_request_notify_t>(nullptr)),
      HasSubstr("cb="));
  for (int v : {static_cast<int>(H5VL_OBJECT_CHANGE_REF_COUNT),
                static_cast<int>(H5VL_OBJECT_EXISTS),
                static_cast<int>(H5VL_OBJECT_LOOKUP),
                static_cast<int>(H5VL_OBJECT_VISIT),
                static_cast<int>(H5VL_OBJECT_FLUSH),
                static_cast<int>(H5VL_OBJECT_REFRESH), 99}) {
    EXPECT_THAT(FormatArgs("Vo", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_REQUEST_GET_ERR_STACK),
                static_cast<int>(H5VL_REQUEST_GET_EXEC_TIME), 99}) {
    EXPECT_THAT(FormatArgs("Vr", "v", v), HasSubstr("v="));
  }
  for (int v : {
#ifndef H5_NO_DEPRECATED_SYMBOLS
           static_cast<int>(H5VL_NATIVE_ATTR_ITERATE_OLD),
#endif
           99}) {
    EXPECT_THAT(FormatArgs("Vs", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_SUBCLS_NONE),
                static_cast<int>(H5VL_SUBCLS_INFO),
                static_cast<int>(H5VL_SUBCLS_WRAP),
                static_cast<int>(H5VL_SUBCLS_ATTR),
                static_cast<int>(H5VL_SUBCLS_DATASET),
                static_cast<int>(H5VL_SUBCLS_DATATYPE),
                static_cast<int>(H5VL_SUBCLS_FILE),
                static_cast<int>(H5VL_SUBCLS_GROUP),
                static_cast<int>(H5VL_SUBCLS_LINK),
                static_cast<int>(H5VL_SUBCLS_OBJECT),
                static_cast<int>(H5VL_SUBCLS_REQUEST),
                static_cast<int>(H5VL_SUBCLS_BLOB),
                static_cast<int>(H5VL_SUBCLS_TOKEN), 99}) {
    EXPECT_THAT(FormatArgs("VS", "v", v), HasSubstr("v="));
  }
  for (int v : {static_cast<int>(H5VL_NATIVE_DATASET_FORMAT_CONVERT),
                static_cast<int>(H5VL_NATIVE_DATASET_GET_CHUNK_INDEX_TYPE),
                static_cast<int>(H5VL_NATIVE_DATASET_GET_CHUNK_STORAGE_SIZE),
                static_cast<int>(H5VL_NATIVE_DATASET_GET_NUM_CHUNKS),
                static_cast<int>(H5VL_NATIVE_DATASET_GET_CHUNK_INFO_BY_IDX),
                static_cast<int>(H5VL_NATIVE_DATASET_GET_CHUNK_INFO_BY_COORD),
                static_cast<int>(H5VL_NATIVE_DATASET_CHUNK_READ),
                static_cast<int>(H5VL_NATIVE_DATASET_CHUNK_WRITE),
                static_cast<int>(H5VL_NATIVE_DATASET_GET_VLEN_BUF_SIZE),
                static_cast<int>(H5VL_NATIVE_DATASET_GET_OFFSET), 99}) {
    EXPECT_THAT(FormatArgs("Vt", "v", v), HasSubstr("v="));
  }
  EXPECT_EQ(FormatArgs("Vu", "v", 12), "v=12");
  for (int v : {static_cast<int>(H5VL_NATIVE_FILE_CLEAR_ELINK_CACHE),
                static_cast<int>(H5VL_NATIVE_FILE_GET_FILE_IMAGE),
                static_cast<int>(H5VL_NATIVE_FILE_GET_FREE_SECTIONS),
                static_cast<int>(H5VL_NATIVE_FILE_GET_FREE_SPACE),
                static_cast<int>(H5VL_NATIVE_FILE_GET_INFO),
                static_cast<int>(H5VL_NATIVE_FILE_GET_MDC_CONF),
                static_cast<int>(H5VL_NATIVE_FILE_GET_MDC_HR),
                static_cast<int>(H5VL_NATIVE_FILE_GET_MDC_SIZE),
                static_cast<int>(H5VL_NATIVE_FILE_GET_SIZE),
                static_cast<int>(H5VL_NATIVE_FILE_GET_VFD_HANDLE),
                static_cast<int>(H5VL_NATIVE_FILE_RESET_MDC_HIT_RATE),
                static_cast<int>(H5VL_NATIVE_FILE_SET_MDC_CONFIG),
                static_cast<int>(H5VL_NATIVE_FILE_GET_METADATA_READ_RETRY_INFO),
                static_cast<int>(H5VL_NATIVE_FILE_START_SWMR_WRITE),
                static_cast<int>(H5VL_NATIVE_FILE_START_MDC_LOGGING),
                static_cast<int>(H5VL_NATIVE_FILE_STOP_MDC_LOGGING),
                static_cast<int>(H5VL_NATIVE_FILE_GET_MDC_LOGGING_STATUS),
                static_cast<int>(H5VL_NATIVE_FILE_FORMAT_CONVERT),
                static_cast<int>(H5VL_NATIVE_FILE_RESET_PAGE_BUFFERING_STATS),
                static_cast<int>(H5VL_NATIVE_FILE_GET_PAGE_BUFFERING_STATS),
                static_cast<int>(H5VL_NATIVE_FILE_GET_MDC_IMAGE_INFO),
                static_cast<int>(H5VL_NATIVE_FILE_GET_EOA),
                static_cast<int>(H5VL_NATIVE_FILE_INCR_FILESIZE),
                static_cast<int>(H5VL_NATIVE_FILE_SET_LIBVER_BOUNDS),
                static_cast<int>(H5VL_NATIVE_FILE_GET_MIN_DSET_OHDR_FLAG),
                static_cast<int>(H5VL_NATIVE_FILE_SET_MIN_DSET_OHDR_FLAG),
                static_cast<int>(H5VL_NATIVE_FILE_POST_OPEN), 99}) {
    EXPECT_THAT(FormatArgs("Vv", "v", v), HasSubstr("v="));
  }
  for (int v : {
#ifndef H5_NO_DEPRECATED_SYMBOLS
           static_cast<int>(H5VL_NATIVE_GROUP_ITERATE_OLD),
           static_cast<int>(H5VL_NATIVE_GROUP_GET_OBJINFO),
#endif
           99}) {
    EXPECT_THAT(FormatArgs("Vw", "v", v), HasSubstr("v="));
  }
  EXPECT_EQ(FormatArgs("Vx", "v", 33), "v=33");
  for (int v : {static_cast<int>(H5VL_NATIVE_OBJECT_GET_COMMENT),
                static_cast<int>(H5VL_NATIVE_OBJECT_SET_COMMENT),
                static_cast<int>(H5VL_NATIVE_OBJECT_DISABLE_MDC_FLUSHES),
                static_cast<int>(H5VL_NATIVE_OBJECT_ENABLE_MDC_FLUSHES),
                static_cast<int>(H5VL_NATIVE_OBJECT_ARE_MDC_FLUSHES_DISABLED),
                static_cast<int>(H5VL_NATIVE_OBJECT_GET_NATIVE_INFO), 99}) {
    EXPECT_THAT(FormatArgs("Vy", "v", v), HasSubstr("v="));
  }
  EXPECT_EQ(FormatArgs("Vz", "v", 44), "v=44");
}

TEST(H5traceTest, InvalidAndReferenceErrorTypesReturnFail) {
  for (const char* bad_type : {"Ax", "Cx", "Dx", "Ex", "Fx", "Gx", "Hx", "Ix",
                               "Lx", "Mx", "Ox", "Px", "Rx", "Sx", "Tx", "Ux",
                               "Vq", "Zx", "Qx", "q"}) {
    std::string out;
    EXPECT_LT(EvalTraceArgs(&out, bad_type, "arg"), 0) << bad_type;
    if (std::strcmp(bad_type, "Mx") == 0) {
      EXPECT_EQ(out, "arg=");
    } else {
      EXPECT_THAT(out, HasSubstr("BADTYPE(")) << bad_type;
    }
  }

  std::string ref_out;
  EXPECT_LT(EvalTraceArgs(&ref_out, "Rd", "r"), 0);
  EXPECT_EQ(ref_out, "r=Reference Region");
  EXPECT_LT(EvalTraceArgs(&ref_out, "Rr", "r"), 0);
  EXPECT_EQ(ref_out, "r=Reference Opaque");
}

}  // namespace
}  // namespace hdf5
