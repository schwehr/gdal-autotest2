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
// Tests internal generic property class path testing helpers
// (third_party/hdf5/src/H5Ptest.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ptest.c

#include <optional>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG; match its internal struct layout.
#ifndef NDEBUG
#define H5PTEST_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5P_FRIEND
#define H5P_TESTING
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Ppkg.h"
}  // extern "C"
#ifdef H5PTEST_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5PTEST_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ScopedPClass = ScopedH5Id<H5Pclose_class>;

std::optional<std::string> GetClassPath(hid_t pclass_id) {
  char* raw = H5P__get_class_path_test(pclass_id);
  if (raw == nullptr) {
    return std::nullopt;
  }
  absl::Cleanup free_raw = [raw] { H5free_memory(raw); };
  return std::string(raw);
}

std::optional<std::string> GetClassName(hid_t pclass_id) {
  char* raw = H5Pget_class_name(pclass_id);
  if (raw == nullptr) {
    return std::nullopt;
  }
  absl::Cleanup free_raw = [raw] { H5free_memory(raw); };
  return std::string(raw);
}

TEST(H5PtestTest, GetClassPathReturnsBuiltInHierarchy) {
  struct BuiltInCase {
    std::string_view expected_path;
    hid_t (*get_class)();
  };

  constexpr BuiltInCase kCases[] = {
      {.expected_path = "root",
       .get_class = []() -> hid_t { return H5P_ROOT; }},
      {.expected_path = "root/object create/group create/file create",
       .get_class = []() -> hid_t { return H5P_FILE_CREATE; }},
      {.expected_path = "root/object copy",
       .get_class = []() -> hid_t { return H5P_OBJECT_COPY; }},
      {.expected_path = "root/string create",
       .get_class = []() -> hid_t { return H5P_STRING_CREATE; }},
      {.expected_path = "root/string create/link create",
       .get_class = []() -> hid_t { return H5P_LINK_CREATE; }},
      {.expected_path = "root/object create/dataset create",
       .get_class = []() -> hid_t { return H5P_DATASET_CREATE; }},
  };

  for (const auto& tc : kCases) {
    std::optional<std::string> path = GetClassPath(tc.get_class());
    ASSERT_TRUE(path.has_value()) << tc.expected_path;
    EXPECT_EQ(*path, tc.expected_path);
  }
}

TEST(H5PtestTest, GetClassPathRejectsInvalidAndNonClassIds) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  ScopedErrorSilence silence;
  EXPECT_EQ(H5P__get_class_path_test(H5I_INVALID_HID), nullptr);
  EXPECT_EQ(H5P__get_class_path_test(H5P_DEFAULT), nullptr);
  EXPECT_EQ(H5P__get_class_path_test(H5P_FILE_CREATE_DEFAULT), nullptr);
  EXPECT_EQ(H5P__get_class_path_test(fcpl.get()), nullptr);
}

TEST(H5PtestTest, OpenClassPathOpensBuiltInAndCustomClassCopies) {
  // 1. Open built-in classes at depth 1, 2, and 3.
  // Note: H5Pequal() compares reference/derived counts, so opened copies
  // (plists == 0, classes == 0, ref_count == 1) differ from built-in classes
  // that already have default plists or derived subclasses. Verify class
  // identity via path, name, property count, and plist instantiation.
  ScopedPClass root_copy(H5P__open_class_path_test("root"));
  ASSERT_TRUE(root_copy.valid());
  EXPECT_EQ(GetClassPath(root_copy.get()), "root");
  EXPECT_EQ(GetClassName(root_copy.get()), "root");

  ScopedPClass ocpy_copy(H5P__open_class_path_test("root/object copy"));
  ASSERT_TRUE(ocpy_copy.valid());
  EXPECT_EQ(GetClassPath(ocpy_copy.get()), "root/object copy");
  EXPECT_EQ(GetClassName(ocpy_copy.get()), "object copy");
  size_t orig_nprops = 0;
  size_t copy_nprops = 0;
  ASSERT_GE(H5Pget_nprops(H5P_OBJECT_COPY, &orig_nprops), 0);
  ASSERT_GE(H5Pget_nprops(ocpy_copy.get(), &copy_nprops), 0);
  EXPECT_EQ(copy_nprops, orig_nprops);
  ScopedPlist ocpy_plist(H5Pcreate(ocpy_copy.get()));
  EXPECT_TRUE(ocpy_plist.valid());

  ScopedPClass lcrt_copy(
      H5P__open_class_path_test("root/string create/link create"));
  ASSERT_TRUE(lcrt_copy.valid());
  EXPECT_EQ(GetClassPath(lcrt_copy.get()), "root/string create/link create");
  EXPECT_EQ(GetClassName(lcrt_copy.get()), "link create");
  ASSERT_GE(H5Pget_nprops(H5P_LINK_CREATE, &orig_nprops), 0);
  ASSERT_GE(H5Pget_nprops(lcrt_copy.get(), &copy_nprops), 0);
  EXPECT_EQ(copy_nprops, orig_nprops);

  // 2. Create custom multi-level class hierarchy with a registered property.
  ScopedPClass parent_cls(H5Pcreate_class(H5P_ROOT, "h5ptest_parent", nullptr,
                                          nullptr, nullptr, nullptr, nullptr,
                                          nullptr));
  ASSERT_TRUE(parent_cls.valid());

  int default_val = 42;
  constexpr char kPropName[] = "custom_int_prop";
  ASSERT_GE(H5Pregister2(parent_cls.get(), kPropName, sizeof(default_val),
                         &default_val, nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr),
            0);

  // Before deriving a subclass, parent_cls has plists == 0 and classes == 0,
  // so H5Pequal() holds against the freshly opened copy.
  ScopedPClass opened_parent(H5P__open_class_path_test("root/h5ptest_parent"));
  ASSERT_TRUE(opened_parent.valid());
  EXPECT_GT(H5Pequal(opened_parent.get(), parent_cls.get()), 0);
  EXPECT_GT(H5Pexist(opened_parent.get(), kPropName), 0);

  ScopedPClass child_cls(H5Pcreate_class(parent_cls.get(), "h5ptest_child",
                                         nullptr, nullptr, nullptr, nullptr,
                                         nullptr, nullptr));
  ASSERT_TRUE(child_cls.valid());

  EXPECT_EQ(GetClassPath(parent_cls.get()), "root/h5ptest_parent");
  EXPECT_EQ(GetClassPath(child_cls.get()), "root/h5ptest_parent/h5ptest_child");

  ScopedPClass opened_child(
      H5P__open_class_path_test("root/h5ptest_parent/h5ptest_child"));
  ASSERT_TRUE(opened_child.valid());
  EXPECT_GT(H5Pequal(opened_child.get(), child_cls.get()), 0);
  EXPECT_EQ(GetClassPath(opened_child.get()),
            "root/h5ptest_parent/h5ptest_child");
  ScopedPlist child_plist(H5Pcreate(opened_child.get()));
  ASSERT_TRUE(child_plist.valid());
  int got_val = 0;
  ASSERT_GE(H5Pget(child_plist.get(), kPropName, &got_val), 0);
  EXPECT_EQ(got_val, 42);
}

TEST(H5PtestTest, OpenClassPathRejectsInvalidAndMissingPaths) {
  ScopedErrorSilence silence;
  EXPECT_EQ(H5P__open_class_path_test(nullptr), H5I_INVALID_HID);
  EXPECT_EQ(H5P__open_class_path_test(""), H5I_INVALID_HID);
  EXPECT_EQ(H5P__open_class_path_test("no_such_root_class"), H5I_INVALID_HID);
  EXPECT_EQ(H5P__open_class_path_test("root/no_such_child_class"),
            H5I_INVALID_HID);
  EXPECT_EQ(H5P__open_class_path_test("no_such_root/file create"),
            H5I_INVALID_HID);
}

TEST(H5PtestTest, GetClassPathHandlesNullPathError) {
  // Construct an isolated root-level H5P_genclass_t whose name is temporarily
  // nulled so H5P__get_class_path() returns NULL, exercising the error path in
  // H5P__get_class_path_test(). Restore name before closing so teardown asserts
  // and frees cleanly.
  H5P_genclass_t* orphan_cls =
      H5P__create_class(nullptr, "h5ptest_orphan", H5P_TYPE_ROOT, nullptr,
                        nullptr, nullptr, nullptr, nullptr, nullptr);
  ASSERT_NE(orphan_cls, nullptr);

  ScopedPClass orphan_id(H5I_register(H5I_GENPROP_CLS, orphan_cls, true));
  if (!orphan_id.valid()) {
    H5P__close_class(orphan_cls);
    ADD_FAILURE() << "H5I_register failed for H5I_GENPROP_CLS";
    return;
  }

  char* saved_name = orphan_cls->name;
  absl::Cleanup restore_name = [&] { orphan_cls->name = saved_name; };
  orphan_cls->name = nullptr;

  ScopedErrorSilence silence;
  EXPECT_EQ(H5P__get_class_path_test(orphan_id.get()), nullptr);
}

}  // namespace
}  // namespace hdf5
