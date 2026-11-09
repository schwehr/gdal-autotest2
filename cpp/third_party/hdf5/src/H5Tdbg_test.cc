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
// Tests internal datatype debug formatting (H5T_debug) and conversion-path
// stats hook (H5T__print_path_stats) across all HDF5 type classes, states,
// byte orders, signs, normalizations, and locations
// (third_party/hdf5/src/H5Tdbg.c).
//
// Note: `:hdf5` is built with -DNDEBUG, so H5T_DEBUG is undefined and
// H5T__print_path_stats compiles to a no-op stub returning SUCCEED.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tdbg.c

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5TDBG_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5T_FRIEND
// H5private.h omits H5__Float16 under __cplusplus, while H5Tpkg.h still
// declares unreferenced extern INF globals with that type.
using H5__Float16 = uint16_t;
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tconv.h"
#include "third_party/hdf5/src/H5Tpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5TDBG_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TDBG_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

std::string CaptureDebug(const H5T_t* dt, herr_t* status_out = nullptr) {
  char* raw = nullptr;
  size_t len = 0;
  FILE* stream = open_memstream(&raw, &len);
  if (stream == nullptr) {
    ADD_FAILURE() << "open_memstream failed";
    if (status_out != nullptr) *status_out = -1;
    return "";
  }
  absl::Cleanup free_raw = [&raw] { std::free(raw); };
  const herr_t rc = H5T_debug(dt, stream);
  std::fclose(stream);
  if (status_out != nullptr) {
    *status_out = rc;
  }
  return raw != nullptr ? std::string(raw, len) : std::string();
}

std::string CaptureDebugId(hid_t type_id, herr_t* status_out = nullptr) {
  const auto* dt =
      static_cast<const H5T_t*>(H5I_object_verify(type_id, H5I_DATATYPE));
  if (dt == nullptr) {
    ADD_FAILURE() << "invalid datatype hid_t";
    if (status_out != nullptr) *status_out = -1;
    return "";
  }
  return CaptureDebug(dt, status_out);
}

TEST(H5TdbgTest, PrintPathStatsReturnsSuccessInNdebugBuild) {
  const auto* int_dt =
      static_cast<const H5T_t*>(H5I_object_verify(H5T_NATIVE_INT, H5I_DATATYPE));
  const auto* dbl_dt = static_cast<const H5T_t*>(
      H5I_object_verify(H5T_NATIVE_DOUBLE, H5I_DATATYPE));
  ASSERT_NE(int_dt, nullptr);
  ASSERT_NE(dbl_dt, nullptr);

  H5T_path_t* path = H5T_path_find(int_dt, dbl_dt);
  ASSERT_NE(path, nullptr);
  int nprint = 0;
  EXPECT_GE(H5T__print_path_stats(path, &nprint), 0);
  EXPECT_GE(H5T__print_path_stats(nullptr, nullptr), 0);
}

TEST(H5TdbgTest, AtomicIntegersFloatsTimeStringBitfieldAndOrders) {
  herr_t rc = -1;

  // Signed & unsigned integers, precision/offset formatting, BE/LE.
  std::string out = CaptureDebugId(H5T_NATIVE_INT, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("int[predefined]"));

  ScopedType uint_be(H5Tcopy(H5T_STD_U32BE));
  ASSERT_GE(uint_be, 0);
  ASSERT_GE(H5Tset_precision(uint_be, 24), 0);
  ASSERT_GE(H5Tset_offset(uint_be, 4), 0);
  out = CaptureDebugId(uint_be, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("int[transient]"));
  EXPECT_THAT(out, HasSubstr("BE"));
  EXPECT_THAT(out, HasSubstr("offset=4"));
  EXPECT_THAT(out, HasSubstr("prec=24"));
  EXPECT_THAT(out, HasSubstr("unsigned"));

  // Floating point norms + 32-bit vs >32-bit ebias + VAX order.
  ScopedType flt_vax(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_GE(flt_vax, 0);
  ASSERT_GE(H5Tset_order(flt_vax, H5T_ORDER_VAX), 0);
  ASSERT_GE(H5Tset_norm(flt_vax, H5T_NORM_NONE), 0);
  ASSERT_GE(H5Tset_ebias(flt_vax, 0x123456789abcdef0ULL), 0);
  out = CaptureDebugId(flt_vax, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("float[transient]"));
  EXPECT_THAT(out, HasSubstr("VAX"));
  EXPECT_THAT(out, HasSubstr("(no-norm)"));
  EXPECT_THAT(out, HasSubstr("bias=0x123456789abcdef0"));

  ScopedType flt_msb(H5Tcopy(H5T_IEEE_F32BE));
  ASSERT_GE(flt_msb, 0);
  ASSERT_GE(H5Tset_norm(flt_msb, H5T_NORM_MSBSET), 0);
  out = CaptureDebugId(flt_msb, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("(msbset)"));
  EXPECT_THAT(out, HasSubstr("bias=0x0000007f"));

  out = CaptureDebugId(H5T_IEEE_F64LE, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("(implied)"));
  EXPECT_THAT(out, HasSubstr("LE"));

  // Time, fixed string (H5T_ORDER_NONE), and bitfield.
  out = CaptureDebugId(H5T_UNIX_D32LE, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("time["));

  out = CaptureDebugId(H5T_C_S1, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("str["));
  EXPECT_THAT(out, HasSubstr("NONE"));

  out = CaptureDebugId(H5T_STD_B16BE, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("bits["));
}

TEST(H5TdbgTest, CompositeOpaqueReferenceEnumVlenArrayComplexAndNamed) {
  herr_t rc = -1;

  // Opaque with tag.
  ScopedType opaque(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_GE(opaque, 0);
  ASSERT_GE(H5Tset_tag(opaque, "dbg_tag"), 0);
  std::string out = CaptureDebugId(opaque, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("opaque[transient]"));
  EXPECT_THAT(out, HasSubstr("tag=\"dbg_tag\""));

  // Compound with multiple members.
  ScopedType cmp(H5Tcreate(H5T_COMPOUND, sizeof(int) + sizeof(double)));
  ASSERT_GE(cmp, 0);
  ASSERT_GE(H5Tinsert(cmp, "a", 0, H5T_NATIVE_INT), 0);
  ASSERT_GE(H5Tinsert(cmp, "b", sizeof(int), H5T_NATIVE_DOUBLE), 0);
  out = CaptureDebugId(cmp, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("struct[transient]"));
  EXPECT_THAT(out, HasSubstr("\"a\" @0 int"));
  EXPECT_THAT(out, HasSubstr("\"b\" @"));

  // Reference types (old object, old region, new object).
  out = CaptureDebugId(H5T_STD_REF_OBJ, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("object reference (old)"));

  out = CaptureDebugId(H5T_STD_REF_DSETREG, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("region reference (old)"));

  out = CaptureDebugId(H5T_STD_REF, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("object reference (new)"));

  // Enum with multiple values (exercises multi-byte hex print).
  ScopedType en(H5Tenum_create(H5T_STD_I16BE));
  ASSERT_GE(en, 0);
  int16_t v0 = 0x1234;
  int16_t v1 = 0x5678;
  ASSERT_GE(H5Tenum_insert(en, "FIRST", &v0), 0);
  ASSERT_GE(H5Tenum_insert(en, "SECOND", &v1), 0);
  out = CaptureDebugId(en, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("enum[transient]"));
  EXPECT_THAT(out, HasSubstr("\"FIRST\" = 0x"));
  EXPECT_THAT(out, HasSubstr("\"SECOND\" = 0x"));

  // VL string & VL sequence (memory and disk locations).
  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_GE(vl_str, 0);
  ASSERT_GE(H5Tset_size(vl_str, H5T_VARIABLE), 0);
  out = CaptureDebugId(vl_str, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("str[transient]"));
  EXPECT_THAT(out, HasSubstr("loc=memory, variable-length"));

  ScopedType vl_seq(H5Tvlen_create(H5T_NATIVE_INT));
  ASSERT_GE(vl_seq, 0);
  out = CaptureDebugId(vl_seq, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("vlen[transient]"));
  EXPECT_THAT(out, HasSubstr("loc=memory VLEN int"));

  ScopedFile file = CreateInMemoryFile("dbg_vlen_disk.h5");
  ASSERT_GE(file, 0);
  H5VL_object_t* file_vol = H5VL_vol_object(file);
  ASSERT_NE(file_vol, nullptr);
  {
    ScopedType vl_disk(H5Tvlen_create(H5T_NATIVE_INT));
    ASSERT_GE(vl_disk, 0);
    auto* vl_disk_dt =
        static_cast<H5T_t*>(H5I_object_verify(vl_disk, H5I_DATATYPE));
    ASSERT_NE(vl_disk_dt, nullptr);
    H5CX_node_t api_ctx{};
    ASSERT_GE(H5CX_push(&api_ctx), 0);
    absl::Cleanup pop_ctx = [] { H5CX_pop(false); };
    ASSERT_GE(H5T_set_loc(vl_disk_dt, file_vol, H5T_LOC_DISK), 0);
    out = CaptureDebug(vl_disk_dt, &rc);
    EXPECT_GE(rc, 0);
    EXPECT_THAT(out, HasSubstr("loc=disk VLEN"));
  }

  // Array falls through to default unknown class message at bottom of H5T_debug.
  constexpr hsize_t kDim = 2;
  ScopedType arr(H5Tarray_create2(H5T_NATIVE_INT, 1, &kDim));
  ASSERT_GE(arr, 0);
  out = CaptureDebugId(arr, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("array[transient]"));
  EXPECT_THAT(out, HasSubstr("unknown class"));

  // Complex type (homogeneous rectangular form).
  ScopedType cplx(H5Tcomplex_create(H5T_NATIVE_FLOAT));
  ASSERT_GE(cplx, 0);
  out = CaptureDebugId(cplx, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("complex number[transient]"));
  EXPECT_THAT(out, HasSubstr("homogeneous, rectangular form"));

  // Committed named open state (via underlying VOL native H5T_t*) and locked
  // RDONLY state.
  ScopedType committed(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_GE(committed, 0);
  ASSERT_GE(H5Tcommit2(file, "named_i", committed, H5P_DEFAULT, H5P_DEFAULT,
                       H5P_DEFAULT),
            0);
  auto* committed_dt =
      static_cast<H5T_t*>(H5I_object_verify(committed, H5I_DATATYPE));
  ASSERT_NE(committed_dt, nullptr);
  ASSERT_NE(committed_dt->vol_obj, nullptr);
  auto* native_committed_dt =
      static_cast<const H5T_t*>(H5VL_object_data(committed_dt->vol_obj));
  ASSERT_NE(native_committed_dt, nullptr);
  out = CaptureDebug(native_committed_dt, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("int[named,open]"));

  ScopedType locked(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_GE(locked, 0);
  ASSERT_GE(H5Tlock(locked), 0);
  out = CaptureDebugId(locked, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("int[predefined]"));
}

TEST(H5TdbgTest, SyntheticEdgeCasesStatesEnumsAndErrorPaths) {
  ScopedErrorSilence silence;
  herr_t rc = 0;

  H5T_shared_t sh{};
  H5T_t dt{};
  dt.shared = &sh;
  sh.size = 4;

  // Read-only constant state (H5T_STATE_RDONLY -> [constant]).
  sh.type = H5T_INTEGER;
  sh.state = H5T_STATE_RDONLY;
  sh.u.atomic.order = H5T_ORDER_LE;
  sh.u.atomic.u.i.sign = H5T_SGN_2;
  std::string out = CaptureDebug(&dt, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("int[constant]"));

  sh.state = H5T_STATE_TRANSIENT;

  // H5T_NO_CLASS error return.
  sh.type = H5T_NO_CLASS;
  (void)CaptureDebug(&dt, &rc);
  EXPECT_LT(rc, 0);

  // Reference subtypes not exposed directly as top-level predefined constants.
  sh.type = H5T_REFERENCE;
  sh.state = H5T_STATE_NAMED;
  sh.u.atomic.prec = 32;
  sh.u.atomic.u.r.rtype = H5R_DATASET_REGION2;
  out = CaptureDebug(&dt, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("region reference (new)[named,closed]"));

  sh.u.atomic.u.r.rtype = H5R_ATTR;
  out = CaptureDebug(&dt, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("attribute reference"));

  sh.u.atomic.u.r.rtype = H5R_BADTYPE;
  out = CaptureDebug(&dt, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("invalid reference"));

  // Order mixed ("order?") & order error on atomic integer type.
  sh.type = H5T_INTEGER;
  sh.u.atomic.u.i.sign = H5T_SGN_2;
  sh.u.atomic.order = H5T_ORDER_MIXED;
  out = CaptureDebug(&dt, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("order?"));

  sh.u.atomic.order = H5T_ORDER_ERROR;
  (void)CaptureDebug(&dt, &rc);
  EXPECT_LT(rc, 0);

  // Integer sign error & unknown sign (H5T_NSGN).
  sh.u.atomic.order = H5T_ORDER_LE;
  sh.u.atomic.u.i.sign = H5T_SGN_ERROR;
  (void)CaptureDebug(&dt, &rc);
  EXPECT_LT(rc, 0);

  sh.u.atomic.u.i.sign = H5T_NSGN;
  out = CaptureDebug(&dt, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("sign?"));

  // Float norm error & unknown norm.
  sh.type = H5T_FLOAT;
  sh.u.atomic.u.f.norm = H5T_NORM_ERROR;
  (void)CaptureDebug(&dt, &rc);
  EXPECT_LT(rc, 0);

  sh.u.atomic.u.f.norm = static_cast<H5T_norm_t>(99);
  out = CaptureDebug(&dt, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("(norm?)"));

  // VLEN unknown location & bad location.
  sh.type = H5T_VLEN;
  sh.u.vlen.type = H5T_VLEN_STRING;
  sh.u.vlen.loc = H5T_LOC_MAXLOC;
  out = CaptureDebug(&dt, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("loc=UNKNOWN"));

  sh.u.vlen.loc = H5T_LOC_BADLOC;
  (void)CaptureDebug(&dt, &rc);
  EXPECT_LT(rc, 0);

  // Complex polar, exponential, and invalid forms.
  sh.type = H5T_COMPLEX;
  sh.u.cplx.form = H5T_COMPLEX_POLAR;
  out = CaptureDebug(&dt, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("polar form"));

  sh.u.cplx.form = H5T_COMPLEX_EXPONENTIAL;
  out = CaptureDebug(&dt, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("exponential form"));

  sh.u.cplx.form = static_cast<H5T_complex_form_t>(99);
  out = CaptureDebug(&dt, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("invalid form"));

  // H5T_NCLASSES enters H5T_IS_ATOMIC -> prints empty class prefix + order?.
  sh.type = H5T_NCLASSES;
  sh.u.atomic.order = H5T_ORDER_MIXED;
  out = CaptureDebug(&dt, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("order?"));
}

}  // namespace
}  // namespace hdf5
