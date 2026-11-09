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
// Tests datatype tree traversal across composite, simple, reference, and
// invalid classes, visit flag combinations, and callback failure propagation
// (third_party/hdf5/src/H5Tvisit.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tvisit.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` compiles with -DNDEBUG; match that macro around internal headers
// so internal struct layouts (e.g., H5T_t, H5T_shared_t) match the library.
#ifndef NDEBUG
#define H5TVISIT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
// H5private.h skips typedef'ing H5__Float16 in C++, yet H5Tpkg.h uses it to
// declare unused internal +/-Inf globals under H5_HAVE__FLOAT16.
typedef uint16_t H5__Float16;
#endif
extern "C" {
#define H5T_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5TVISIT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TVISIT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

struct VisitLog {
  std::vector<H5T_class_t> classes;
  std::vector<const H5T_t*> ptrs;
  int fail_on_call = -1;
};

herr_t RecordVisitCb(H5T_t* dt, void* op_data) {
  auto* log = static_cast<VisitLog*>(op_data);
  const int call_idx = static_cast<int>(log->classes.size());
  log->classes.push_back(dt->shared->type);
  log->ptrs.push_back(dt);
  if (call_idx == log->fail_on_call) {
    return -1;
  }
  return 0;
}

H5T_t* LookupType(hid_t tid) {
  return static_cast<H5T_t*>(H5I_object_verify(tid, H5I_DATATYPE));
}

TEST(H5TvisitTest, VisitsSimpleClassesOnlyWhenSimpleFlagSet) {
  ASSERT_GE(H5open(), 0);

  ScopedType fixed_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(fixed_str.valid());
  ASSERT_GE(H5Tset_size(fixed_str.get(), 4), 0);

  ScopedType opaque(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(opaque.valid());

  struct SimpleCase {
    hid_t tid;
    H5T_class_t expected_class;
  };
  const std::array<SimpleCase, 6> kCases = {{
      {.tid = H5T_NATIVE_INT32, .expected_class = H5T_INTEGER},
      {.tid = H5T_NATIVE_DOUBLE, .expected_class = H5T_FLOAT},
      {.tid = H5T_UNIX_D64LE, .expected_class = H5T_TIME},
      {.tid = fixed_str.get(), .expected_class = H5T_STRING},
      {.tid = H5T_NATIVE_B32, .expected_class = H5T_BITFIELD},
      {.tid = opaque.get(), .expected_class = H5T_OPAQUE},
  }};

  for (const auto& tc : kCases) {
    H5T_t* dt = LookupType(tc.tid);
    ASSERT_NE(dt, nullptr);

    // Without H5T_VISIT_SIMPLE, simple types are not visited even when
    // composite flags are set.
    for (unsigned flags :
         {0u, static_cast<unsigned>(H5T_VISIT_COMPOSITE_FIRST),
          static_cast<unsigned>(H5T_VISIT_COMPOSITE_LAST)}) {
      VisitLog log;
      EXPECT_GE(H5T__visit(dt, flags, RecordVisitCb, &log), 0);
      EXPECT_THAT(log.classes, IsEmpty());
    }

    VisitLog simple_log;
    EXPECT_GE(H5T__visit(dt, H5T_VISIT_SIMPLE, RecordVisitCb, &simple_log), 0);
    EXPECT_THAT(simple_log.classes, ElementsAre(tc.expected_class));
    EXPECT_THAT(simple_log.ptrs, ElementsAre(dt));
  }
}

TEST(H5TvisitTest, ReferenceClassIsBothCompositeAndSimple) {
  ASSERT_GE(H5open(), 0);

  for (hid_t ref_tid : {H5T_STD_REF, H5T_STD_REF_OBJ, H5T_STD_REF_DSETREG}) {
    H5T_t* dt = LookupType(ref_tid);
    ASSERT_NE(dt, nullptr);

    // H5T_IS_COMPOSITE(H5T_REFERENCE) is true AND the switch statement handles
    // H5T_REFERENCE in the simple branch, so all three flags fire independently.
    VisitLog first_log;
    EXPECT_GE(
        H5T__visit(dt, H5T_VISIT_COMPOSITE_FIRST, RecordVisitCb, &first_log),
        0);
    EXPECT_THAT(first_log.classes, ElementsAre(H5T_REFERENCE));

    VisitLog last_log;
    EXPECT_GE(
        H5T__visit(dt, H5T_VISIT_COMPOSITE_LAST, RecordVisitCb, &last_log), 0);
    EXPECT_THAT(last_log.classes, ElementsAre(H5T_REFERENCE));

    VisitLog simple_log;
    EXPECT_GE(H5T__visit(dt, H5T_VISIT_SIMPLE, RecordVisitCb, &simple_log), 0);
    EXPECT_THAT(simple_log.classes, ElementsAre(H5T_REFERENCE));

    VisitLog all_log;
    EXPECT_GE(H5T__visit(dt,
                         H5T_VISIT_COMPOSITE_FIRST | H5T_VISIT_SIMPLE |
                             H5T_VISIT_COMPOSITE_LAST,
                         RecordVisitCb, &all_log),
              0);
    EXPECT_THAT(all_log.classes,
                ElementsAre(H5T_REFERENCE, H5T_REFERENCE, H5T_REFERENCE));
  }
}

TEST(H5TvisitTest, VisitsCompositeFirstLastBothAndSimpleOnlyOrders) {
  ASSERT_GE(H5open(), 0);

  // 1. Build a nested composite datatype covering all composite classes:
  //    COMPOUND { "arr": ARRAY[2](ENUM<INT16>),
  //               "vlen": VLEN(COMPLEX<FLOAT>),
  //               "vstr": VLEN_STR(UCHAR),
  //               "s": STRING(4) }
  ScopedType enum_type(H5Tenum_create(H5T_NATIVE_INT16));
  ASSERT_TRUE(enum_type.valid());
  const int16_t enum_val = 7;
  ASSERT_GE(H5Tenum_insert(enum_type.get(), "SEVEN", &enum_val), 0);

  constexpr hsize_t kArrDim = 2;
  ScopedType arr_type(H5Tarray_create2(enum_type.get(), 1, &kArrDim));
  ASSERT_TRUE(arr_type.valid());

  ScopedType cplx_type(H5Tcomplex_create(H5T_NATIVE_FLOAT));
  ASSERT_TRUE(cplx_type.valid());

  ScopedType vlen_type(H5Tvlen_create(cplx_type.get()));
  ASSERT_TRUE(vlen_type.valid());

  ScopedType vstr_type(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vstr_type.valid());
  ASSERT_GE(H5Tset_size(vstr_type.get(), H5T_VARIABLE), 0);

  ScopedType fixed_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(fixed_str.valid());
  ASSERT_GE(H5Tset_size(fixed_str.get(), 4), 0);

  const size_t arr_sz = H5Tget_size(arr_type.get());
  const size_t vlen_sz = H5Tget_size(vlen_type.get());
  const size_t vstr_sz = H5Tget_size(vstr_type.get());
  const size_t fstr_sz = H5Tget_size(fixed_str.get());
  ScopedType comp_type(
      H5Tcreate(H5T_COMPOUND, arr_sz + vlen_sz + vstr_sz + fstr_sz));
  ASSERT_TRUE(comp_type.valid());
  ASSERT_GE(H5Tinsert(comp_type.get(), "arr", 0, arr_type.get()), 0);
  ASSERT_GE(H5Tinsert(comp_type.get(), "vlen", arr_sz, vlen_type.get()), 0);
  ASSERT_GE(
      H5Tinsert(comp_type.get(), "vstr", arr_sz + vlen_sz, vstr_type.get()), 0);
  ASSERT_GE(H5Tinsert(comp_type.get(), "s", arr_sz + vlen_sz + vstr_sz,
                      fixed_str.get()),
            0);

  H5T_t* comp_dt = LookupType(comp_type.get());
  ASSERT_NE(comp_dt, nullptr);

  // 2. Simple leaves only: INT16 (under ARRAY->ENUM), FLOAT (under
  //    VLEN->COMPLEX), UCHAR (parent of VLEN_STR), STRING.
  VisitLog simple_only;
  EXPECT_GE(H5T__visit(comp_dt, H5T_VISIT_SIMPLE, RecordVisitCb, &simple_only),
            0);
  EXPECT_THAT(simple_only.classes,
              ElementsAre(H5T_INTEGER, H5T_FLOAT, H5T_INTEGER, H5T_STRING));

  // 3. Composite-first (pre-order composites, skipping simple leaves).
  VisitLog comp_first;
  EXPECT_GE(
      H5T__visit(comp_dt, H5T_VISIT_COMPOSITE_FIRST, RecordVisitCb, &comp_first),
      0);
  EXPECT_THAT(comp_first.classes,
              ElementsAre(H5T_COMPOUND, H5T_ARRAY, H5T_ENUM, H5T_VLEN,
                          H5T_COMPLEX, H5T_VLEN));

  // 4. Composite-last (post-order composites, skipping simple leaves).
  VisitLog comp_last;
  EXPECT_GE(
      H5T__visit(comp_dt, H5T_VISIT_COMPOSITE_LAST, RecordVisitCb, &comp_last),
      0);
  EXPECT_THAT(comp_last.classes,
              ElementsAre(H5T_ENUM, H5T_ARRAY, H5T_COMPLEX, H5T_VLEN, H5T_VLEN,
                          H5T_COMPOUND));

  // 5. Full pre-order tree (composite-first + simple).
  VisitLog pre_order;
  EXPECT_GE(H5T__visit(comp_dt, H5T_VISIT_COMPOSITE_FIRST | H5T_VISIT_SIMPLE,
                       RecordVisitCb, &pre_order),
            0);
  EXPECT_THAT(pre_order.classes,
              ElementsAre(H5T_COMPOUND, H5T_ARRAY, H5T_ENUM, H5T_INTEGER,
                          H5T_VLEN, H5T_COMPLEX, H5T_FLOAT, H5T_VLEN,
                          H5T_INTEGER, H5T_STRING));

  // 6. Full post-order tree (composite-last + simple).
  VisitLog post_order;
  EXPECT_GE(H5T__visit(comp_dt, H5T_VISIT_COMPOSITE_LAST | H5T_VISIT_SIMPLE,
                       RecordVisitCb, &post_order),
            0);
  EXPECT_THAT(post_order.classes,
              ElementsAre(H5T_INTEGER, H5T_ENUM, H5T_ARRAY, H5T_FLOAT,
                          H5T_COMPLEX, H5T_VLEN, H5T_INTEGER, H5T_VLEN,
                          H5T_STRING, H5T_COMPOUND));

  // 7. Euler tour (both composite flags + simple visits composites twice).
  VisitLog euler;
  EXPECT_GE(H5T__visit(comp_dt,
                       H5T_VISIT_COMPOSITE_FIRST | H5T_VISIT_COMPOSITE_LAST |
                           H5T_VISIT_SIMPLE,
                       RecordVisitCb, &euler),
            0);
  EXPECT_THAT(
      euler.classes,
      ElementsAre(H5T_COMPOUND, H5T_ARRAY, H5T_ENUM, H5T_INTEGER, H5T_ENUM,
                  H5T_ARRAY, H5T_VLEN, H5T_COMPLEX, H5T_FLOAT, H5T_COMPLEX,
                  H5T_VLEN, H5T_VLEN, H5T_INTEGER, H5T_VLEN, H5T_STRING,
                  H5T_COMPOUND));

  // 8. Empty compound datatype (0 members) visits only itself.
  ScopedType empty_comp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(empty_comp.valid());
  VisitLog empty_log;
  EXPECT_GE(H5T__visit(LookupType(empty_comp.get()),
                       H5T_VISIT_COMPOSITE_FIRST | H5T_VISIT_SIMPLE,
                       RecordVisitCb, &empty_log),
            0);
  EXPECT_THAT(empty_log.classes, ElementsAre(H5T_COMPOUND));
}

TEST(H5TvisitTest, CallbackFailuresPropagateFromEveryBranch) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  ScopedType vlen_type(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vlen_type.valid());
  H5T_t* vlen_dt = LookupType(vlen_type.get());
  ASSERT_NE(vlen_dt, nullptr);

  // 1. COMPOSITE_FIRST failure on root composite stops before visiting parent.
  {
    VisitLog log{.fail_on_call = 0};
    EXPECT_LT(
        H5T__visit(vlen_dt, H5T_VISIT_COMPOSITE_FIRST | H5T_VISIT_SIMPLE,
                   RecordVisitCb, &log),
        0);
    EXPECT_THAT(log.classes, ElementsAre(H5T_VLEN));
  }

  // 2. Parent failure under VLEN stops before COMPOSITE_LAST executes.
  {
    VisitLog log{.fail_on_call = 0};
    EXPECT_LT(
        H5T__visit(vlen_dt, H5T_VISIT_SIMPLE | H5T_VISIT_COMPOSITE_LAST,
                   RecordVisitCb, &log),
        0);
    EXPECT_THAT(log.classes, ElementsAre(H5T_INTEGER));
  }

  // 3. COMPOSITE_LAST failure on root composite after parent succeeds.
  {
    VisitLog log{.fail_on_call = 1};
    EXPECT_LT(
        H5T__visit(vlen_dt, H5T_VISIT_SIMPLE | H5T_VISIT_COMPOSITE_LAST,
                   RecordVisitCb, &log),
        0);
    EXPECT_THAT(log.classes, ElementsAre(H5T_INTEGER, H5T_VLEN));
  }

  // 4. Simple leaf failure directly on a non-composite datatype.
  {
    VisitLog log{.fail_on_call = 0};
    EXPECT_LT(
        H5T__visit(LookupType(H5T_NATIVE_INT32), H5T_VISIT_SIMPLE,
                   RecordVisitCb, &log),
        0);
    EXPECT_THAT(log.classes, ElementsAre(H5T_INTEGER));
  }

  // 5. Compound member failure aborts before visiting subsequent members.
  ScopedType comp(H5Tcreate(H5T_COMPOUND, 16));
  ASSERT_TRUE(comp.valid());
  ASSERT_GE(H5Tinsert(comp.get(), "a", 0, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(comp.get(), "b", 8, H5T_NATIVE_DOUBLE), 0);
  {
    VisitLog log{.fail_on_call = 0};
    EXPECT_LT(
        H5T__visit(LookupType(comp.get()),
                   H5T_VISIT_SIMPLE | H5T_VISIT_COMPOSITE_LAST,
                   RecordVisitCb, &log),
        0);
    EXPECT_THAT(log.classes, ElementsAre(H5T_INTEGER));
  }
}

TEST(H5TvisitTest, InvalidDatatypeClassesFail) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  ScopedType opaque(H5Tcreate(H5T_OPAQUE, 1));
  ASSERT_TRUE(opaque.valid());
  H5T_t* dt = LookupType(opaque.get());
  ASSERT_NE(dt, nullptr);

  const H5T_class_t orig_class = dt->shared->type;
  absl::Cleanup restore_type = [&] { dt->shared->type = orig_class; };

  for (H5T_class_t bad_class : {H5T_NO_CLASS, H5T_NCLASSES}) {
    dt->shared->type = bad_class;
    VisitLog log;
    EXPECT_LT(H5T__visit(dt,
                         H5T_VISIT_COMPOSITE_FIRST | H5T_VISIT_SIMPLE |
                             H5T_VISIT_COMPOSITE_LAST,
                         RecordVisitCb, &log),
              0);
    EXPECT_THAT(log.classes, IsEmpty());
  }
}

TEST(H5TvisitTest, UpgradeVersionViaCommitExercisesVisit) {
  ASSERT_GE(H5open(), 0);

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);
  ScopedFile file(
      H5Fcreate("visit_upgrade.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedType comp(H5Tcreate(H5T_COMPOUND, sizeof(int32_t)));
  ASSERT_TRUE(comp.valid());
  ASSERT_GE(H5Tinsert(comp.get(), "x", 0, H5T_NATIVE_INT32), 0);

  EXPECT_GE(H5Tcommit2(file.get(), "committed_comp", comp.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
}

}  // namespace
}  // namespace hdf5
