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
// Tests dynamic optional VOL operation registration, lookup, unregistration,
// and termination (third_party/hdf5/src/H5VLdyn_ops.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLdyn_ops.c

#ifndef NDEBUG
#define H5VL_DYN_OPS_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5VL_FRIEND
#include "third_party/hdf5/src/H5VLpkg.h"
}  // extern "C"

#ifdef H5VL_DYN_OPS_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5VL_DYN_OPS_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <array>
#include <cstddef>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5VLdynOpsTest, PublicApiRejectsInvalidArguments) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup term_ops = [] { (void)H5VL__term_opt_operation(); };

  ScopedErrorSilence silence;
  int op_val = -1;

  EXPECT_LT(H5VLregister_opt_operation(H5VL_SUBCLS_DATASET, "op", nullptr), 0);
  EXPECT_LT(H5VLregister_opt_operation(H5VL_SUBCLS_DATASET, nullptr, &op_val),
            0);
  EXPECT_LT(H5VLregister_opt_operation(H5VL_SUBCLS_DATASET, "", &op_val), 0);

  EXPECT_LT(H5VLfind_opt_operation(H5VL_SUBCLS_DATASET, "op", nullptr), 0);
  EXPECT_LT(H5VLfind_opt_operation(H5VL_SUBCLS_DATASET, nullptr, &op_val), 0);
  EXPECT_LT(H5VLfind_opt_operation(H5VL_SUBCLS_DATASET, "", &op_val), 0);

  EXPECT_LT(H5VLunregister_opt_operation(H5VL_SUBCLS_DATASET, nullptr), 0);
  EXPECT_LT(H5VLunregister_opt_operation(H5VL_SUBCLS_DATASET, ""), 0);

  constexpr std::array<H5VL_subclass_t, 5> kDisallowedPublicSubclasses = {
      H5VL_SUBCLS_NONE, H5VL_SUBCLS_INFO, H5VL_SUBCLS_WRAP, H5VL_SUBCLS_BLOB,
      H5VL_SUBCLS_TOKEN};
  for (H5VL_subclass_t subcls : kDisallowedPublicSubclasses) {
    EXPECT_LT(H5VLregister_opt_operation(subcls, "org.google.disallowed", &op_val),
              0);
    EXPECT_LT(H5VLfind_opt_operation(subcls, "org.google.disallowed", &op_val),
              0);
    EXPECT_LT(H5VLunregister_opt_operation(subcls, "org.google.disallowed"), 0);
  }
}

TEST(H5VLdynOpsTest, RegisterFindDuplicateAndUnregisterLifecycle) {
  ASSERT_GE(H5open(), 0);
  ASSERT_GE(H5VL__term_opt_operation(), 0);
  absl::Cleanup term_ops = [] { (void)H5VL__term_opt_operation(); };

  EXPECT_EQ(H5VL__num_opt_operation(), 0u);

  // Looking up or unregistering on a subclass with a NULL skip list fails.
  {
    ScopedErrorSilence silence;
    int missing_val = -1;
    EXPECT_LT(H5VLfind_opt_operation(H5VL_SUBCLS_DATASET,
                                     "org.google.dynops.missing", &missing_val),
              0);
    EXPECT_LT(H5VLunregister_opt_operation(H5VL_SUBCLS_DATASET,
                                           "org.google.dynops.missing"),
              0);
  }

  // Register first operation; creates skip list and allocates op_val >= 1024.
  int op_val1 = -1;
  ASSERT_GE(H5VLregister_opt_operation(H5VL_SUBCLS_DATASET,
                                       "org.google.dynops.op1", &op_val1),
            0);
  EXPECT_GE(op_val1, H5VL_RESERVED_NATIVE_OPTIONAL);
  EXPECT_EQ(H5VL__num_opt_operation(), 1u);

  // Duplicate registration in populated skip list returns an error.
  {
    ScopedErrorSilence silence;
    int dup_val = -1;
    EXPECT_LT(H5VLregister_opt_operation(H5VL_SUBCLS_DATASET,
                                         "org.google.dynops.op1", &dup_val),
              0);
    EXPECT_EQ(H5VL__num_opt_operation(), 1u);

    // Lookup and unregister of absent name on non-NULL skip list also fail.
    EXPECT_LT(H5VLfind_opt_operation(H5VL_SUBCLS_DATASET,
                                     "org.google.dynops.absent", &dup_val),
              0);
    EXPECT_LT(H5VLunregister_opt_operation(H5VL_SUBCLS_DATASET,
                                           "org.google.dynops.absent"),
              0);
  }

  // Register a second operation in the same subclass; value increments by 1.
  int op_val2 = -1;
  ASSERT_GE(H5VLregister_opt_operation(H5VL_SUBCLS_DATASET,
                                       "org.google.dynops.op2", &op_val2),
            0);
  EXPECT_EQ(op_val2, op_val1 + 1);
  EXPECT_EQ(H5VL__num_opt_operation(), 2u);

  // Both registered operations are discoverable via find.
  int found_val = -1;
  ASSERT_GE(H5VLfind_opt_operation(H5VL_SUBCLS_DATASET,
                                   "org.google.dynops.op1", &found_val),
            0);
  EXPECT_EQ(found_val, op_val1);
  ASSERT_GE(H5VLfind_opt_operation(H5VL_SUBCLS_DATASET,
                                   "org.google.dynops.op2", &found_val),
            0);
  EXPECT_EQ(found_val, op_val2);

  // Unregistering op1 leaves op2 active (skip list remains open).
  ASSERT_GE(
      H5VLunregister_opt_operation(H5VL_SUBCLS_DATASET, "org.google.dynops.op1"),
      0);
  EXPECT_EQ(H5VL__num_opt_operation(), 1u);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VLfind_opt_operation(H5VL_SUBCLS_DATASET,
                                     "org.google.dynops.op1", &found_val),
              0);
  }
  ASSERT_GE(H5VLfind_opt_operation(H5VL_SUBCLS_DATASET,
                                   "org.google.dynops.op2", &found_val),
            0);
  EXPECT_EQ(found_val, op_val2);

  // Unregistering the last op closes and clears the subclass skip list.
  ASSERT_GE(
      H5VLunregister_opt_operation(H5VL_SUBCLS_DATASET, "org.google.dynops.op2"),
      0);
  EXPECT_EQ(H5VL__num_opt_operation(), 0u);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VLfind_opt_operation(H5VL_SUBCLS_DATASET,
                                     "org.google.dynops.op2", &found_val),
              0);
  }

  // Re-registering recreates the skip list cleanly and assigns next value.
  int op_val3 = -1;
  ASSERT_GE(H5VLregister_opt_operation(H5VL_SUBCLS_DATASET,
                                       "org.google.dynops.op1", &op_val3),
            0);
  EXPECT_EQ(op_val3, op_val2 + 1);
  EXPECT_EQ(H5VL__num_opt_operation(), 1u);
}

TEST(H5VLdynOpsTest, AllSubclassesAndTermOperationCleanlyReleaseAllNodes) {
  ASSERT_GE(H5open(), 0);
  ASSERT_GE(H5VL__term_opt_operation(), 0);
  absl::Cleanup term_ops = [] { (void)H5VL__term_opt_operation(); };

  constexpr std::array<H5VL_subclass_t, 13> kAllSubclasses = {
      H5VL_SUBCLS_NONE,    H5VL_SUBCLS_INFO,   H5VL_SUBCLS_WRAP,
      H5VL_SUBCLS_ATTR,    H5VL_SUBCLS_DATASET, H5VL_SUBCLS_DATATYPE,
      H5VL_SUBCLS_FILE,    H5VL_SUBCLS_GROUP,  H5VL_SUBCLS_LINK,
      H5VL_SUBCLS_OBJECT,  H5VL_SUBCLS_REQUEST, H5VL_SUBCLS_BLOB,
      H5VL_SUBCLS_TOKEN};

  // Internal miss checks on empty skip list.
  {
    ScopedErrorSilence silence;
    int missing = -1;
    EXPECT_LT(
        H5VL__find_opt_operation(H5VL_SUBCLS_BLOB, "org.google.none", &missing),
        0);
    EXPECT_LT(H5VL__unregister_opt_operation(H5VL_SUBCLS_BLOB, "org.google.none"),
              0);
  }

  std::array<int, kAllSubclasses.size()> registered_vals{};
  for (size_t i = 0; i < kAllSubclasses.size(); ++i) {
    const std::string name = "org.google.subcls." + std::to_string(i);
    ASSERT_GE(H5VL__register_opt_operation(kAllSubclasses[i], name.c_str(),
                                           &registered_vals[i]),
              0);
    EXPECT_GE(registered_vals[i], H5VL_RESERVED_NATIVE_OPTIONAL);
  }
  EXPECT_EQ(H5VL__num_opt_operation(), kAllSubclasses.size());

  // Verify lookup for each subclass and test internal miss with non-NULL list.
  for (size_t i = 0; i < kAllSubclasses.size(); ++i) {
    const std::string name = "org.google.subcls." + std::to_string(i);
    int found = -1;
    ASSERT_GE(
        H5VL__find_opt_operation(kAllSubclasses[i], name.c_str(), &found), 0);
    EXPECT_EQ(found, registered_vals[i]);
  }
  {
    ScopedErrorSilence silence;
    int missing = -1;
    EXPECT_LT(H5VL__find_opt_operation(H5VL_SUBCLS_BLOB,
                                       "org.google.other_missing", &missing),
              0);
  }

  // Terminating cleans every subclass skip list and frees all nodes.
  ASSERT_GE(H5VL__term_opt_operation(), 0);
  EXPECT_EQ(H5VL__num_opt_operation(), 0u);

  for (size_t i = 0; i < kAllSubclasses.size(); ++i) {
    ScopedErrorSilence silence;
    const std::string name = "org.google.subcls." + std::to_string(i);
    int found = -1;
    EXPECT_LT(H5VL__find_opt_operation(kAllSubclasses[i], name.c_str(), &found),
              0);
  }
}

TEST(H5VLdynOpsTest, LibraryCloseTerminatesRemainingDynamicOps) {
  ASSERT_GE(H5open(), 0);
  ASSERT_GE(H5VL__term_opt_operation(), 0);

  int file_op = -1;
  int group_op = -1;
  ASSERT_GE(
      H5VLregister_opt_operation(H5VL_SUBCLS_FILE, "org.google.close.fop",
                                 &file_op),
      0);
  ASSERT_GE(
      H5VLregister_opt_operation(H5VL_SUBCLS_GROUP, "org.google.close.gop",
                                 &group_op),
      0);
  EXPECT_EQ(H5VL__num_opt_operation(), 2u);

  // H5close invokes H5VL_term_package -> H5VL__term_opt_operation.
  ASSERT_GE(H5close(), 0);
  ASSERT_GE(H5open(), 0);

  EXPECT_EQ(H5VL__num_opt_operation(), 0u);
  int found = -1;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5VLfind_opt_operation(H5VL_SUBCLS_FILE, "org.google.close.fop", &found),
        0);
    EXPECT_LT(H5VLfind_opt_operation(H5VL_SUBCLS_GROUP, "org.google.close.gop",
                                     &found),
              0);
  }
}

}  // namespace
}  // namespace hdf5
