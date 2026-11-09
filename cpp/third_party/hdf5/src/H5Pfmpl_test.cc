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
// Tests the file mount property list class (H5P_FILE_MOUNT) registration,
// property defaults, copying/comparison, mounting usage, and internal property
// registration error handling (third_party/hdf5/src/H5Pfmpl.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pfmpl.c

#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG; match internal struct layouts cleanly.
#ifndef NDEBUG
#define H5PFMPL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5P_FRIEND
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Ppkg.h"
H5_DLLVAR const H5P_libclass_t H5P_CLS_FMNT[1];
}  // extern "C"
#ifdef H5PFMPL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5PFMPL_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

TEST(H5PfmplTest, ClassMetadataHierarchyAndDefaultPropertyValues) {
  ASSERT_GE(H5open(), 0);

  // Verify the internal H5P_CLS_FMNT registration table entry directly.
  EXPECT_STREQ(H5P_CLS_FMNT[0].name, "file mount");
  EXPECT_EQ(H5P_CLS_FMNT[0].type, H5P_TYPE_FILE_MOUNT);
  ASSERT_NE(H5P_CLS_FMNT[0].reg_prop_func, nullptr);

  char* cls_name = H5Pget_class_name(H5P_FILE_MOUNT);
  ASSERT_NE(cls_name, nullptr);
  absl::Cleanup free_cls_name = [&] { H5free_memory(cls_name); };
  EXPECT_STREQ(cls_name, "file mount");

  ScopedPlist parent_cls(H5Pget_class_parent(H5P_FILE_MOUNT));
  ASSERT_GE(parent_cls.get(), 0);
  EXPECT_GT(H5Pequal(parent_cls.get(), H5P_ROOT), 0);

  // Default property list belongs to H5P_FILE_MOUNT and contains "local".
  EXPECT_GT(H5Pisa_class(H5P_FILE_MOUNT_DEFAULT, H5P_FILE_MOUNT), 0);
  EXPECT_GT(H5Pexist(H5P_FILE_MOUNT, H5F_MNT_SYM_LOCAL_NAME), 0);

  size_t class_nprops = 0;
  ASSERT_GE(H5Pget_nprops(H5P_FILE_MOUNT, &class_nprops), 0);
  EXPECT_GE(class_nprops, 1u);

  ScopedPlist fmpl(H5Pcreate(H5P_FILE_MOUNT));
  ASSERT_GE(fmpl.get(), 0);
  EXPECT_GT(H5Pisa_class(fmpl.get(), H5P_FILE_MOUNT), 0);
  EXPECT_GT(H5Pexist(fmpl.get(), H5F_MNT_SYM_LOCAL_NAME), 0);

  size_t prop_size = 0;
  ASSERT_GE(H5Pget_size(fmpl.get(), H5F_MNT_SYM_LOCAL_NAME, &prop_size), 0);
  EXPECT_EQ(prop_size, sizeof(bool));

  bool local_flag = true;
  ASSERT_GE(H5Pget(fmpl.get(), H5F_MNT_SYM_LOCAL_NAME, &local_flag), 0);
  EXPECT_FALSE(local_flag);

  EXPECT_GT(H5Pequal(fmpl.get(), H5P_FILE_MOUNT_DEFAULT), 0);
}

TEST(H5PfmplTest, ModifyCopyEncodeAndUseWithInMemoryMount) {
  ScopedPlist fmpl(H5Pcreate(H5P_FILE_MOUNT));
  ASSERT_GE(fmpl.get(), 0);

  // Toggle "local" property and verify copy/equality behavior.
  bool local_flag = true;
  ASSERT_GE(H5Pset(fmpl.get(), H5F_MNT_SYM_LOCAL_NAME, &local_flag), 0);
  EXPECT_EQ(H5Pequal(fmpl.get(), H5P_FILE_MOUNT_DEFAULT), 0);

  ScopedPlist copied(H5Pcopy(fmpl.get()));
  ASSERT_GE(copied.get(), 0);
  EXPECT_GT(H5Pequal(fmpl.get(), copied.get()), 0);

  bool read_flag = false;
  ASSERT_GE(H5Pget(copied.get(), H5F_MNT_SYM_LOCAL_NAME, &read_flag), 0);
  EXPECT_TRUE(read_flag);

  // Encode/decode exercises property list serialization of file-mount lists.
  size_t enc_size = 0;
  ASSERT_GE(H5Pencode2(fmpl.get(), nullptr, &enc_size, H5P_DEFAULT), 0);
  ASSERT_GT(enc_size, 0u);
  std::vector<uint8_t> enc_buf(enc_size, 0);
  ASSERT_GE(H5Pencode2(fmpl.get(), enc_buf.data(), &enc_size, H5P_DEFAULT), 0);

  ScopedPlist decoded(H5Pdecode(enc_buf.data()));
  ASSERT_GE(decoded.get(), 0);
  EXPECT_GT(H5Pisa_class(decoded.get(), H5P_FILE_MOUNT), 0);

  // Use the file mount property list with in-memory Core VFD files.
  ScopedFile parent_file = CreateInMemoryFile("fmpl_parent.h5");
  ASSERT_GE(parent_file.get(), 0);
  ScopedFile child_file = CreateInMemoryFile("fmpl_child.h5");
  ASSERT_GE(child_file.get(), 0);

  ScopedGroup mount_grp(H5Gcreate2(parent_file.get(), "/mnt", H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(mount_grp.get(), 0);
  ScopedGroup child_sub(H5Gcreate2(child_file.get(), "/inside", H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(child_sub.get(), 0);
  child_sub.reset();
  mount_grp.reset();

  ASSERT_GE(H5Fmount(parent_file.get(), "/mnt", child_file.get(), fmpl.get()),
            0);
  EXPECT_GT(H5Lexists(parent_file.get(), "/mnt/inside", H5P_DEFAULT), 0);
  EXPECT_GE(H5Funmount(parent_file.get(), "/mnt"), 0);
}

TEST(H5PfmplTest, InternalRegPropSucceedsThenFailsOnDuplicateRegistration) {
  ASSERT_GE(H5open(), 0);

  // Create an isolated temporary property class rooted at H5P_CLS_ROOT_g so
  // global H5P_FILE_MOUNT state remains completely unmodified.
  H5P_genclass_t* tmp_cls =
      H5P__create_class(H5P_CLS_ROOT_g, "tmp_fmnt_class", H5P_TYPE_FILE_MOUNT,
                        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  ASSERT_NE(tmp_cls, nullptr);
  absl::Cleanup close_tmp_cls = [&] { H5P__close_class(tmp_cls); };

  ASSERT_NE(H5P_CLS_FMNT[0].reg_prop_func, nullptr);
  ASSERT_GE(H5P_CLS_FMNT[0].reg_prop_func(tmp_cls), 0);

  EXPECT_GT(H5P__exist_pclass(tmp_cls, H5F_MNT_SYM_LOCAL_NAME), 0);
  bool def_local = true;
  ASSERT_GE(H5P__class_get(tmp_cls, H5F_MNT_SYM_LOCAL_NAME, &def_local), 0);
  EXPECT_FALSE(def_local);

  // Calling reg_prop_func again hits H5P__register_real's duplicate-name error.
  ScopedErrorSilence silence;
  EXPECT_LT(H5P_CLS_FMNT[0].reg_prop_func(tmp_cls), 0);
}

}  // namespace
}  // namespace hdf5
