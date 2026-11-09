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
// Tests generic property list and property list class APIs: copying, class
// creation/hierarchy, property registration/insertion/removal/unregistration,
// value get/set, iteration, encode/decode, and equality checks
// (third_party/hdf5/src/H5P.c).
//
// Build/feature caveats: Tests run in the default google3 non-MPI Linux build.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5P.c

#include "third_party/hdf5/src/hdf5.h"

#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ::testing::UnorderedElementsAre;

using ScopedPlistClass = ScopedH5Id<H5Pclose_class>;

struct ClassCallbackTracker {
  int create_calls = 0;
  int copy_calls = 0;
  int close_calls = 0;
  bool fail_create = false;
  bool fail_copy = false;
  bool fail_close = false;
};

herr_t ClsCreateCb(hid_t /*prop_id*/, void* data) {
  auto* tracker = static_cast<ClassCallbackTracker*>(data);
  if (!tracker) return -1;
  ++tracker->create_calls;
  return tracker->fail_create ? -1 : 0;
}

herr_t ClsCopyCb(hid_t /*new_id*/, hid_t /*old_id*/, void* data) {
  auto* tracker = static_cast<ClassCallbackTracker*>(data);
  if (!tracker) return -1;
  ++tracker->copy_calls;
  return tracker->fail_copy ? -1 : 0;
}

herr_t ClsCloseCb(hid_t /*prop_id*/, void* data) {
  auto* tracker = static_cast<ClassCallbackTracker*>(data);
  if (!tracker) return -1;
  ++tracker->close_calls;
  return tracker->fail_close ? -1 : 0;
}

// Mode codes embedded inside CallbackProp::mode let property callbacks remain
// 100% stateless (thread-safe under TSan and isolated across Testify orders).
enum CallbackMode : int32_t {
  kModeNormal = 0,
  kModeMutateCreate = 1,
  kModeFailCreate = -1,
  kModeMutateSet = 2,
  kModeFailSet = -2,
  kModeMutateGet = 3,
  kModeFailGet = -3,
  kModeFailDelete = -4,
  kModeMutateCopy = 5,
};

struct CallbackProp {
  int32_t value = 0;
  int32_t mode = kModeNormal;
};

herr_t PropCreateCb(const char* /*name*/, size_t /*size*/, void* value) {
  auto* prop = static_cast<CallbackProp*>(value);
  if (!prop) return -1;
  if (prop->mode == kModeFailCreate) return -1;
  if (prop->mode == kModeMutateCreate) prop->value += 100;
  return 0;
}

herr_t PropSetCb(hid_t /*prop_id*/, const char* /*name*/, size_t /*size*/,
                 void* value) {
  auto* prop = static_cast<CallbackProp*>(value);
  if (!prop) return -1;
  if (prop->mode == kModeFailSet) return -1;
  if (prop->mode == kModeMutateSet) prop->value *= 2;
  return 0;
}

herr_t PropGetCb(hid_t /*prop_id*/, const char* /*name*/, size_t /*size*/,
                 void* value) {
  auto* prop = static_cast<CallbackProp*>(value);
  if (!prop) return -1;
  if (prop->mode == kModeFailGet) return -1;
  if (prop->mode == kModeMutateGet) prop->value += 1000;
  return 0;
}

herr_t PropDeleteCb(hid_t /*prop_id*/, const char* /*name*/, size_t /*size*/,
                    void* value) {
  auto* prop = static_cast<CallbackProp*>(value);
  if (!prop) return -1;
  return prop->mode == kModeFailDelete ? -1 : 0;
}

herr_t PropCopyCb(const char* /*name*/, size_t /*size*/, void* value) {
  auto* prop = static_cast<CallbackProp*>(value);
  if (!prop) return -1;
  if (prop->mode == kModeMutateCopy) prop->value += 10;
  return 0;
}

int PropCompareCb(const void* value1, const void* value2, size_t /*size*/) {
  const auto* a = static_cast<const CallbackProp*>(value1);
  const auto* b = static_cast<const CallbackProp*>(value2);
  if (!a || !b) return 0;
  if (a->value < b->value) return -1;
  if (a->value > b->value) return 1;
  if (a->mode < b->mode) return -1;
  if (a->mode > b->mode) return 1;
  return 0;
}

herr_t PropCloseCb(const char* /*name*/, size_t /*size*/, void* /*value*/) {
  return 0;
}

struct IterCollector {
  std::vector<std::string> names;
  int stop_after = -1;
  int return_code = 0;
};

herr_t CollectPropsIterCb(hid_t /*id*/, const char* name, void* iter_data) {
  auto* collector = static_cast<IterCollector*>(iter_data);
  if (!collector || !name) return -1;
  collector->names.emplace_back(name);
  if (collector->stop_after > 0 &&
      static_cast<int>(collector->names.size()) >= collector->stop_after) {
    return collector->return_code;
  }
  return 0;
}

TEST(H5PTest, CreateClassHierarchyCallbacksAndErrors) {
  ASSERT_GE(H5open(), 0);
  int dummy_data = 1;

  // 1. Validate invalid parent, empty/null name, and unmatched data/callbacks.
  {
    ScopedErrorSilence silence;
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());

    EXPECT_EQ(H5Pcreate_class(H5I_INVALID_HID, "bad", nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr),
              H5I_INVALID_HID);
    EXPECT_EQ(H5Pcreate_class(dcpl.get(), "bad", nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr),
              H5I_INVALID_HID);
    EXPECT_EQ(H5Pcreate_class(H5P_ROOT, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr),
              H5I_INVALID_HID);
    EXPECT_EQ(H5Pcreate_class(H5P_ROOT, "", nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr),
              H5I_INVALID_HID);
    EXPECT_EQ(H5Pcreate_class(H5P_ROOT, "bad_create_data", nullptr, &dummy_data,
                              nullptr, nullptr, nullptr, nullptr),
              H5I_INVALID_HID);
    EXPECT_EQ(H5Pcreate_class(H5P_ROOT, "bad_copy_data", nullptr, nullptr,
                              nullptr, &dummy_data, nullptr, nullptr),
              H5I_INVALID_HID);
    EXPECT_EQ(H5Pcreate_class(H5P_ROOT, "bad_close_data", nullptr, nullptr,
                              nullptr, nullptr, nullptr, &dummy_data),
              H5I_INVALID_HID);

    // Invalid argument checks for create/class-query/close APIs.
    EXPECT_EQ(H5Pcreate(H5I_INVALID_HID), H5I_INVALID_HID);
    EXPECT_EQ(H5Pcreate(dcpl.get()), H5I_INVALID_HID);
    EXPECT_EQ(H5Pget_class(H5I_INVALID_HID), H5I_INVALID_HID);
    EXPECT_EQ(H5Pget_class(H5P_ROOT), H5I_INVALID_HID);
    EXPECT_EQ(H5Pget_class_name(H5I_INVALID_HID), nullptr);
    EXPECT_EQ(H5Pget_class_name(dcpl.get()), nullptr);
    EXPECT_EQ(H5Pget_class_parent(H5I_INVALID_HID), H5I_INVALID_HID);
    EXPECT_EQ(H5Pget_class_parent(dcpl.get()), H5I_INVALID_HID);
    EXPECT_EQ(H5Pget_class_parent(H5P_ROOT), H5I_INVALID_HID);
    EXPECT_LT(H5Pclose(H5P_ROOT), 0);
    EXPECT_LT(H5Pclose(H5I_INVALID_HID), 0);
    EXPECT_LT(H5Pclose_class(dcpl.get()), 0);
    EXPECT_LT(H5Pclose_class(H5I_INVALID_HID), 0);
  }

  // H5Pclose(H5P_DEFAULT) is explicitly a no-op success.
  EXPECT_GE(H5Pclose(H5P_DEFAULT), 0);

  // 2. Create custom parent + derived child classes and verify callbacks.
  // Also verify H5P_DEFAULT as parent creates a rootless class cleanly.
  ScopedPlistClass default_parent_cls(
      H5Pcreate_class(H5P_DEFAULT, "h5p_default_parent_cls", nullptr, nullptr,
                      nullptr, nullptr, nullptr, nullptr));
  ASSERT_TRUE(default_parent_cls.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Pget_class_parent(default_parent_cls.get()), H5I_INVALID_HID);
  }

  ClassCallbackTracker tracker;
  ScopedPlistClass parent_cls(H5Pcreate_class(
      H5P_ROOT, "h5p_parent_cls", ClsCreateCb, &tracker, ClsCopyCb, &tracker,
      ClsCloseCb, &tracker));
  ASSERT_TRUE(parent_cls.valid());

  // Inherited create/close callbacks receive parent class user data; copy is
  // exercised on parent_plist (H5Pint.c H5P_copy_plist passes pclass->copy_data).
  ScopedPlistClass child_cls(
      H5Pcreate_class(parent_cls.get(), "h5p_child_cls", nullptr, nullptr,
                      nullptr, nullptr, nullptr, nullptr));
  ASSERT_TRUE(child_cls.valid());

  char* child_name = H5Pget_class_name(child_cls.get());
  ASSERT_NE(child_name, nullptr);
  absl::Cleanup free_child_name = [child_name] { H5free_memory(child_name); };
  EXPECT_STREQ(child_name, "h5p_child_cls");

  ScopedPlistClass fetched_parent(H5Pget_class_parent(child_cls.get()));
  ASSERT_TRUE(fetched_parent.valid());
  EXPECT_GT(H5Pequal(fetched_parent.get(), parent_cls.get()), 0);

  {
    ScopedPlist child_plist(H5Pcreate(child_cls.get()));
    ASSERT_TRUE(child_plist.valid());
    EXPECT_EQ(tracker.create_calls, 1);

    ScopedPlistClass plist_cls(H5Pget_class(child_plist.get()));
    ASSERT_TRUE(plist_cls.valid());
    EXPECT_GT(H5Pequal(plist_cls.get(), child_cls.get()), 0);

    ScopedPlist parent_plist(H5Pcreate(parent_cls.get()));
    ASSERT_TRUE(parent_plist.valid());
    EXPECT_EQ(tracker.create_calls, 2);

    ScopedPlist copied(H5Pcopy(parent_plist.get()));
    ASSERT_TRUE(copied.valid());
    EXPECT_EQ(tracker.copy_calls, 1);
  }
  EXPECT_EQ(tracker.close_calls, 3);

  // 3. Class create callback returning negative makes H5Pcreate fail cleanly.
  tracker.fail_create = true;
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Pcreate(parent_cls.get()), H5I_INVALID_HID);
  }
}

TEST(H5PTest, CopyHandlesDefaultListsClassesAndFailures) {
  EXPECT_EQ(H5Pcopy(H5P_DEFAULT), H5P_DEFAULT);

  ClassCallbackTracker tracker;
  ScopedPlistClass cls(H5Pcreate_class(H5P_ROOT, "h5p_copy_cls", ClsCreateCb,
                                       &tracker, ClsCopyCb, &tracker,
                                       ClsCloseCb, &tracker));
  ASSERT_TRUE(cls.valid());

  CallbackProp prop_def{7, kModeNormal};
  constexpr char kCopyProp[] = "copy_prop";
  ASSERT_GE(H5Pregister2(cls.get(), kCopyProp, sizeof(CallbackProp), &prop_def,
                         nullptr, nullptr, nullptr, nullptr, PropCopyCb,
                         PropCompareCb, nullptr),
            0);

  // Class copy returns valid equivalent class ID.
  ScopedPlistClass cls_copy(H5Pcopy(cls.get()));
  ASSERT_TRUE(cls_copy.valid());
  EXPECT_GT(H5Pequal(cls.get(), cls_copy.get()), 0);

  // Negative copy checks: bad ID, non-property ID, and cls_copy failure.
  // Note: prp_copy failure inside H5P_copy_plist returns before INC_LST yet
  // calls H5P_close -> DEC_LST in upstream H5Pint.c, so we trigger H5P_copy_plist
  // failure via ClsCopyCb (which executes after INC_LST and unwinds cleanly).
  ScopedPlist plist(H5Pcreate(cls.get()));
  ASSERT_TRUE(plist.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  tracker.fail_copy = true;
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Pcopy(H5I_INVALID_HID), H5I_INVALID_HID);
    EXPECT_EQ(H5Pcopy(space.get()), H5I_INVALID_HID);
    EXPECT_EQ(H5Pcopy(plist.get()), H5I_INVALID_HID);
  }
}

TEST(H5PTest, RegisterPropertiesCallbacksZeroSizedAndClassSplitting) {
  ScopedPlistClass cls(H5Pcreate_class(H5P_ROOT, "h5p_reg_cls", nullptr,
                                       nullptr, nullptr, nullptr, nullptr,
                                       nullptr));
  ASSERT_TRUE(cls.valid());

  CallbackProp def_prop{10, kModeMutateCreate};
  constexpr char kPropA[] = "prop_a";
  constexpr char kZeroFlag[] = "zero_flag";

  // 1. Validate H5Pregister2 error arguments.
  {
    ScopedErrorSilence silence;
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    EXPECT_LT(H5Pregister2(H5I_INVALID_HID, kPropA, sizeof(CallbackProp),
                           &def_prop, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pregister2(dcpl.get(), kPropA, sizeof(CallbackProp), &def_prop,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr),
              0);
    EXPECT_LT(H5Pregister2(cls.get(), nullptr, sizeof(CallbackProp), &def_prop,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr),
              0);
    EXPECT_LT(H5Pregister2(cls.get(), "", sizeof(CallbackProp), &def_prop,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr),
              0);
    EXPECT_LT(H5Pregister2(cls.get(), kPropA, sizeof(CallbackProp), nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr),
              0);
  }

  // 2. Register sized property with all callbacks and zero-sized flag property.
  ASSERT_GE(H5Pregister2(cls.get(), kPropA, sizeof(CallbackProp), &def_prop,
                         PropCreateCb, PropSetCb, PropGetCb, PropDeleteCb,
                         PropCopyCb, PropCompareCb, PropCloseCb),
            0);
  ASSERT_GE(H5Pregister2(cls.get(), kZeroFlag, 0, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
            0);

  // Duplicate property registration must fail inside H5P__register.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pregister2(cls.get(), kPropA, sizeof(CallbackProp), &def_prop,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr),
              0);
  }

  // 3. Instantiate a plist so class has pclass->plists > 0, then register again
  // to exercise class splitting & H5I_subst (H5P.c:427-439).
  ScopedPlist pre_split_plist(H5Pcreate(cls.get()));
  ASSERT_TRUE(pre_split_plist.valid());

  int32_t split_def = 55;
  constexpr char kSplitProp[] = "split_prop";
  ASSERT_GE(H5Pregister2(cls.get(), kSplitProp, sizeof(split_def), &split_def,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr),
            0);
  EXPECT_GT(H5Pexist(cls.get(), kSplitProp), 0);
  EXPECT_EQ(H5Pexist(pre_split_plist.get(), kSplitProp), 0);

  ScopedPlist post_split_plist(H5Pcreate(cls.get()));
  ASSERT_TRUE(post_split_plist.valid());
  EXPECT_GT(H5Pexist(post_split_plist.get(), kSplitProp), 0);

  // Verify create callback mutated initial value (10 + 100 = 110).
  CallbackProp out{};
  ASSERT_GE(H5Pget(post_split_plist.get(), kPropA, &out), 0);
  EXPECT_EQ(out.value, 110);

  // Exercise set/get callback mutation and failure paths.
  CallbackProp set_mut{7, kModeMutateSet};
  ASSERT_GE(H5Pset(post_split_plist.get(), kPropA, &set_mut), 0);
  ASSERT_GE(H5Pget(post_split_plist.get(), kPropA, &out), 0);
  EXPECT_EQ(out.value, 14);

  CallbackProp get_mut{9, kModeMutateGet};
  ASSERT_GE(H5Pset(post_split_plist.get(), kPropA, &get_mut), 0);
  ASSERT_GE(H5Pget(post_split_plist.get(), kPropA, &out), 0);
  EXPECT_EQ(out.value, 1009);

  // Zero-sized property queries succeed for exist/size, fail for set/get.
  size_t prop_size = 999;
  EXPECT_GT(H5Pexist(cls.get(), kZeroFlag), 0);
  EXPECT_GT(H5Pexist(post_split_plist.get(), kZeroFlag), 0);
  ASSERT_GE(H5Pget_size(cls.get(), kZeroFlag, &prop_size), 0);
  EXPECT_EQ(prop_size, 0U);
  ASSERT_GE(H5Pget_size(post_split_plist.get(), kZeroFlag, &prop_size), 0);
  EXPECT_EQ(prop_size, 0U);

  {
    ScopedErrorSilence silence;
    CallbackProp fail_set{999, kModeFailSet};
    EXPECT_LT(H5Pset(post_split_plist.get(), kPropA, &fail_set), 0);
    // Stored value remains unchanged after failing set callback.
    ASSERT_GE(H5Pget(post_split_plist.get(), kPropA, &out), 0);
    EXPECT_EQ(out.value, 1009);

    CallbackProp fail_get{0, kModeFailGet};
    ASSERT_GE(H5Pset(post_split_plist.get(), kPropA, &fail_get), 0);
    EXPECT_LT(H5Pget(post_split_plist.get(), kPropA, &out), 0);

    int32_t dummy = 1;
    EXPECT_LT(H5Pset(post_split_plist.get(), kZeroFlag, &dummy), 0);
    EXPECT_LT(H5Pget(post_split_plist.get(), kZeroFlag, &dummy), 0);
  }

  // Test registering property when class has a failing create callback.
  CallbackProp fail_create_def{1, kModeFailCreate};
  constexpr char kFailCreateProp[] = "fail_create_prop";
  ASSERT_GE(H5Pregister2(cls.get(), kFailCreateProp, sizeof(CallbackProp),
                         &fail_create_def, PropCreateCb, nullptr, nullptr,
                         nullptr, nullptr, PropCompareCb, nullptr),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Pcreate(cls.get()), H5I_INVALID_HID);
  }
}

TEST(H5PTest, InsertSetGetExistSizeNpropsRemoveAndUnregister) {
  ScopedPlistClass cls(H5Pcreate_class(H5P_ROOT, "h5p_ops_cls", nullptr,
                                       nullptr, nullptr, nullptr, nullptr,
                                       nullptr));
  ASSERT_TRUE(cls.valid());

  int32_t cls_val = 42;
  constexpr char kClsProp[] = "cls_prop";
  ASSERT_GE(H5Pregister2(cls.get(), kClsProp, sizeof(cls_val), &cls_val,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr),
            0);

  ScopedPlist plist(H5Pcreate(cls.get()));
  ASSERT_TRUE(plist.valid());

  constexpr char kTmpProp[] = "tmp_prop";
  constexpr char kZeroTmp[] = "zero_tmp";
  CallbackProp tmp_init{12, kModeNormal};

  // 1. Validate error paths for insert/set/get/exist/size/nprops/remove/unreg.
  {
    ScopedErrorSilence silence;
    ScopedSpace space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());

    EXPECT_LT(H5Pinsert2(H5I_INVALID_HID, kTmpProp, sizeof(tmp_init), &tmp_init,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pinsert2(cls.get(), kTmpProp, sizeof(tmp_init), &tmp_init,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pinsert2(plist.get(), nullptr, sizeof(tmp_init), &tmp_init,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pinsert2(plist.get(), "", sizeof(tmp_init), &tmp_init, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pinsert2(plist.get(), kTmpProp, sizeof(tmp_init), nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    // Duplicate against inherited class property fails H5P_insert.
    EXPECT_LT(H5Pinsert2(plist.get(), kClsProp, sizeof(tmp_init), &tmp_init,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
              0);

    EXPECT_LT(H5Pset(H5I_INVALID_HID, kClsProp, &cls_val), 0);
    EXPECT_LT(H5Pset(cls.get(), kClsProp, &cls_val), 0);
    EXPECT_LT(H5Pset(plist.get(), nullptr, &cls_val), 0);
    EXPECT_LT(H5Pset(plist.get(), "", &cls_val), 0);
    EXPECT_LT(H5Pset(plist.get(), kClsProp, nullptr), 0);
    EXPECT_LT(H5Pset(plist.get(), "missing_prop", &cls_val), 0);

    int32_t out_i = 0;
    EXPECT_LT(H5Pget(H5I_INVALID_HID, kClsProp, &out_i), 0);
    EXPECT_LT(H5Pget(cls.get(), kClsProp, &out_i), 0);
    EXPECT_LT(H5Pget(plist.get(), nullptr, &out_i), 0);
    EXPECT_LT(H5Pget(plist.get(), "", &out_i), 0);
    EXPECT_LT(H5Pget(plist.get(), kClsProp, nullptr), 0);
    EXPECT_LT(H5Pget(plist.get(), "missing_prop", &out_i), 0);

    EXPECT_LT(H5Pexist(H5I_INVALID_HID, kClsProp), 0);
    EXPECT_LT(H5Pexist(space.get(), kClsProp), 0);
    EXPECT_LT(H5Pexist(plist.get(), nullptr), 0);
    EXPECT_LT(H5Pexist(plist.get(), ""), 0);

    size_t sz = 0;
    EXPECT_LT(H5Pget_size(H5I_INVALID_HID, kClsProp, &sz), 0);
    EXPECT_LT(H5Pget_size(space.get(), kClsProp, &sz), 0);
    EXPECT_LT(H5Pget_size(plist.get(), nullptr, &sz), 0);
    EXPECT_LT(H5Pget_size(plist.get(), "", &sz), 0);
    EXPECT_LT(H5Pget_size(plist.get(), kClsProp, nullptr), 0);
    EXPECT_LT(H5Pget_size(plist.get(), "missing_prop", &sz), 0);
    EXPECT_LT(H5Pget_size(cls.get(), "missing_prop", &sz), 0);

    size_t nprops = 0;
    EXPECT_LT(H5Pget_nprops(H5I_INVALID_HID, &nprops), 0);
    EXPECT_LT(H5Pget_nprops(space.get(), &nprops), 0);
    EXPECT_LT(H5Pget_nprops(plist.get(), nullptr), 0);

    EXPECT_LT(H5Premove(H5I_INVALID_HID, kClsProp), 0);
    EXPECT_LT(H5Premove(cls.get(), kClsProp), 0);
    EXPECT_LT(H5Premove(plist.get(), nullptr), 0);
    EXPECT_LT(H5Premove(plist.get(), ""), 0);
    EXPECT_LT(H5Premove(plist.get(), "missing_prop"), 0);

    EXPECT_LT(H5Punregister(H5I_INVALID_HID, kClsProp), 0);
    EXPECT_LT(H5Punregister(plist.get(), kClsProp), 0);
    EXPECT_LT(H5Punregister(cls.get(), nullptr), 0);
    EXPECT_LT(H5Punregister(cls.get(), ""), 0);
    EXPECT_LT(H5Punregister(cls.get(), "missing_prop"), 0);
  }

  // 2. Normal insert, query, remove, and unregister lifecycle.
  ASSERT_GE(H5Pinsert2(plist.get(), kTmpProp, sizeof(CallbackProp), &tmp_init,
                       PropSetCb, PropGetCb, PropDeleteCb, PropCopyCb,
                       PropCompareCb, PropCloseCb),
            0);
  ASSERT_GE(H5Pinsert2(plist.get(), kZeroTmp, 0, nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr, PropCloseCb),
            0);

  EXPECT_GT(H5Pexist(plist.get(), kTmpProp), 0);
  EXPECT_EQ(H5Pexist(cls.get(), kTmpProp), 0);

  size_t prop_size = 0;
  ASSERT_GE(H5Pget_size(plist.get(), kTmpProp, &prop_size), 0);
  EXPECT_EQ(prop_size, sizeof(CallbackProp));
  ASSERT_GE(H5Pget_size(cls.get(), kClsProp, &prop_size), 0);
  EXPECT_EQ(prop_size, sizeof(cls_val));

  size_t cls_nprops = 0;
  size_t plist_nprops = 0;
  ASSERT_GE(H5Pget_nprops(cls.get(), &cls_nprops), 0);
  ASSERT_GE(H5Pget_nprops(plist.get(), &plist_nprops), 0);
  EXPECT_EQ(cls_nprops, 1U);
  EXPECT_EQ(plist_nprops, 3U);

  // Remove kTmpProp cleanly, then test failing prp_delete on a separate
  // inserted property (note: H5P__set_plist_cb invokes prp_delete on old value).
  ASSERT_GE(H5Premove(plist.get(), kTmpProp), 0);
  EXPECT_EQ(H5Pexist(plist.get(), kTmpProp), 0);

  CallbackProp fail_del{12, kModeFailDelete};
  constexpr char kFailDelProp[] = "fail_del_prop";
  ASSERT_GE(H5Pinsert2(plist.get(), kFailDelProp, sizeof(CallbackProp),
                       &fail_del, PropSetCb, PropGetCb, PropDeleteCb,
                       PropCopyCb, PropCompareCb, PropCloseCb),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Premove(plist.get(), kFailDelProp), 0);
  }
  EXPECT_GT(H5Pexist(plist.get(), kFailDelProp), 0);

  // Exercise H5Pset/H5Pget on kClsProp, close plist (so no shared_name pointer
  // outlives H5Punregister), then unregister kClsProp from cls and verify both
  // existing default-valued lists and future lists lack it.
  int32_t mod_val = 99;
  ASSERT_GE(H5Pset(plist.get(), kClsProp, &mod_val), 0);
  int32_t out_mod = 0;
  ASSERT_GE(H5Pget(plist.get(), kClsProp, &out_mod), 0);
  EXPECT_EQ(out_mod, 99);
  plist.reset();

  ScopedPlist existing_default_plist(H5Pcreate(cls.get()));
  ASSERT_TRUE(existing_default_plist.valid());
  EXPECT_GT(H5Pexist(existing_default_plist.get(), kClsProp), 0);

  ASSERT_GE(H5Punregister(cls.get(), kClsProp), 0);
  EXPECT_EQ(H5Pexist(cls.get(), kClsProp), 0);
  EXPECT_EQ(H5Pexist(existing_default_plist.get(), kClsProp), 0);

  ScopedPlist future_plist(H5Pcreate(cls.get()));
  ASSERT_TRUE(future_plist.valid());
  EXPECT_EQ(H5Pexist(future_plist.get(), kClsProp), 0);
}

TEST(H5PTest, CopyPropEqualIsaClassAndIterate) {
  ScopedPlistClass cls1(H5Pcreate_class(H5P_ROOT, "h5p_iter_cls1", nullptr,
                                        nullptr, nullptr, nullptr, nullptr,
                                        nullptr));
  ASSERT_TRUE(cls1.valid());
  ScopedPlistClass cls2(H5Pcreate_class(H5P_ROOT, "h5p_iter_cls2", nullptr,
                                        nullptr, nullptr, nullptr, nullptr,
                                        nullptr));
  ASSERT_TRUE(cls2.valid());

  CallbackProp prop1{5, kModeNormal};
  CallbackProp prop2{10, kModeMutateCopy};
  constexpr char kSharedProp[] = "shared_prop";
  constexpr char kExtraProp[] = "extra_prop";
  ASSERT_GE(H5Pregister2(cls1.get(), kSharedProp, sizeof(CallbackProp), &prop1,
                         PropCreateCb, PropSetCb, PropGetCb, PropDeleteCb,
                         PropCopyCb, PropCompareCb, PropCloseCb),
            0);
  ASSERT_GE(H5Pregister2(cls1.get(), kExtraProp, sizeof(CallbackProp), &prop2,
                         PropCreateCb, PropSetCb, PropGetCb, PropDeleteCb,
                         PropCopyCb, PropCompareCb, PropCloseCb),
            0);
  ASSERT_GE(H5Pregister2(cls2.get(), kSharedProp, sizeof(CallbackProp), &prop1,
                         PropCreateCb, PropSetCb, PropGetCb, PropDeleteCb,
                         PropCopyCb, PropCompareCb, PropCloseCb),
            0);

  ScopedPlist list1(H5Pcreate(cls1.get()));
  ASSERT_TRUE(list1.valid());
  ScopedPlist list2(H5Pcreate(cls2.get()));
  ASSERT_TRUE(list2.valid());

  // 1. H5Pisa_class checks.
  EXPECT_GT(H5Pisa_class(list1.get(), cls1.get()), 0);
  EXPECT_GT(H5Pisa_class(list1.get(), H5P_ROOT), 0);
  EXPECT_EQ(H5Pisa_class(list1.get(), cls2.get()), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pisa_class(cls1.get(), cls1.get()), 0);
    EXPECT_LT(H5Pisa_class(list1.get(), list1.get()), 0);
  }

  // 2. H5Pequal checks (classes and lists, equal, unequal, error).
  EXPECT_EQ(H5Pequal(cls1.get(), cls2.get()), 0);
  EXPECT_GT(H5Pequal(cls1.get(), cls1.get()), 0);

  ScopedPlist list1_copy(H5Pcopy(list1.get()));
  ASSERT_TRUE(list1_copy.valid());
  // Note: kExtraProp has mode kModeMutateCopy, so list1_copy has value 20 != 10.
  EXPECT_EQ(H5Pequal(list1.get(), list1_copy.get()), 0);
  ASSERT_GE(H5Pset(list1_copy.get(), kExtraProp, &prop2), 0);
  EXPECT_GT(H5Pequal(list1.get(), list1_copy.get()), 0);

  {
    ScopedErrorSilence silence;
    ScopedSpace space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());
    EXPECT_LT(H5Pequal(H5I_INVALID_HID, list1.get()), 0);
    EXPECT_LT(H5Pequal(list1.get(), H5I_INVALID_HID), 0);
    EXPECT_LT(H5Pequal(space.get(), list1.get()), 0);
    EXPECT_LT(H5Pequal(list1.get(), cls1.get()), 0);
  }

  // 3. H5Pcopy_prop between lists (copy missing & overwrite existing) and
  // between classes.
  CallbackProp updated_extra{25, kModeMutateCreate};
  ASSERT_GE(H5Pset(list1.get(), kExtraProp, &updated_extra), 0);
  ASSERT_GE(H5Pcopy_prop(list2.get(), list1.get(), kExtraProp), 0);
  CallbackProp out{};
  ASSERT_GE(H5Pget(list2.get(), kExtraProp, &out), 0);
  EXPECT_EQ(out.value, 125);  // Mutated by PropCreateCb (+100) on insert.
  EXPECT_GT(H5Pexist(list2.get(), kExtraProp), 0);

  CallbackProp updated_shared{42, kModeMutateCopy};
  ASSERT_GE(H5Pset(list1.get(), kSharedProp, &updated_shared), 0);
  ASSERT_GE(H5Pcopy_prop(list2.get(), list1.get(), kSharedProp), 0);

  // Class-to-class copy (missing and overwrite).
  ASSERT_GE(H5Pcopy_prop(cls2.get(), cls1.get(), kSharedProp), 0);
  ASSERT_GE(H5Pcopy_prop(cls2.get(), cls1.get(), kExtraProp), 0);
  EXPECT_GT(H5Pexist(cls2.get(), kExtraProp), 0);

  {
    ScopedErrorSilence silence;
    ScopedSpace space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());
    EXPECT_LT(H5Pcopy_prop(list2.get(), H5I_INVALID_HID, kSharedProp), 0);
    EXPECT_LT(H5Pcopy_prop(H5I_INVALID_HID, list1.get(), kSharedProp), 0);
    EXPECT_LT(H5Pcopy_prop(list2.get(), space.get(), kSharedProp), 0);
    EXPECT_LT(H5Pcopy_prop(space.get(), list1.get(), kSharedProp), 0);
    EXPECT_LT(H5Pcopy_prop(cls2.get(), list1.get(), kSharedProp), 0);
    EXPECT_LT(H5Pcopy_prop(list2.get(), list1.get(), nullptr), 0);
    EXPECT_LT(H5Pcopy_prop(list2.get(), list1.get(), ""), 0);
    EXPECT_LT(H5Pcopy_prop(list2.get(), list1.get(), "nonexistent"), 0);
    EXPECT_LT(H5Pcopy_prop(cls2.get(), cls1.get(), "nonexistent"), 0);
  }

  // 4. H5Piterate over plist and class (full, idx==NULL, pause/resume, error).
  {
    IterCollector all_plist;
    EXPECT_EQ(H5Piterate(list1.get(), nullptr, CollectPropsIterCb, &all_plist),
              0);
    EXPECT_THAT(all_plist.names, UnorderedElementsAre(kSharedProp, kExtraProp));

    IterCollector split_plist{.stop_after = 1, .return_code = 7};
    int idx = 0;
    EXPECT_EQ(H5Piterate(list1.get(), &idx, CollectPropsIterCb, &split_plist),
              7);
    // H5Piterate leaves idx at the element that returned non-zero.
    EXPECT_EQ(idx, 0);
    ASSERT_EQ(split_plist.names.size(), 1U);
    const std::string paused_prop = split_plist.names[0];
    split_plist.names.clear();
    split_plist.stop_after = -1;
    idx = 1;
    EXPECT_EQ(H5Piterate(list1.get(), &idx, CollectPropsIterCb, &split_plist),
              0);
    EXPECT_EQ(idx, 2);
    ASSERT_EQ(split_plist.names.size(), 1U);
    EXPECT_THAT((std::vector<std::string>{paused_prop, split_plist.names[0]}),
                UnorderedElementsAre(kSharedProp, kExtraProp));

    IterCollector err_plist{.stop_after = 1, .return_code = -3};
    int err_idx = 0;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5Piterate(list1.get(), &err_idx, CollectPropsIterCb, &err_plist), 0);
    }

    IterCollector all_cls;
    EXPECT_EQ(H5Piterate(cls1.get(), nullptr, CollectPropsIterCb, &all_cls), 0);
    EXPECT_THAT(all_cls.names, UnorderedElementsAre(kSharedProp, kExtraProp));

    IterCollector err_cls{.stop_after = 1, .return_code = -4};
    int cls_idx = 0;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Piterate(cls1.get(), &cls_idx, CollectPropsIterCb, &err_cls),
                0);
    }

    {
      ScopedErrorSilence silence;
      ScopedSpace space(H5Screate(H5S_SCALAR));
      ASSERT_TRUE(space.valid());
      EXPECT_LT(H5Piterate(H5I_INVALID_HID, nullptr, CollectPropsIterCb,
                           &all_plist),
                0);
      EXPECT_LT(
          H5Piterate(space.get(), nullptr, CollectPropsIterCb, &all_plist), 0);
      EXPECT_LT(H5Piterate(list1.get(), nullptr, nullptr, nullptr), 0);
    }
  }
}

TEST(H5PTest, EncodeDecodeRoundTripAndMalformedInputs) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  const hsize_t chunk_dims[2] = {4, 8};
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, chunk_dims), 0);

  size_t encoded_size = 0;
  ASSERT_GE(H5Pencode2(dcpl.get(), nullptr, &encoded_size, fapl.get()), 0);
  ASSERT_GT(encoded_size, 2U);

  std::vector<uint8_t> buf(encoded_size);
  ASSERT_GE(H5Pencode2(dcpl.get(), buf.data(), &encoded_size, fapl.get()), 0);

  ScopedPlist decoded(H5Pdecode(buf.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(dcpl.get(), decoded.get()), 0);

  // Negative encode/decode tests.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pencode2(H5I_INVALID_HID, nullptr, &encoded_size, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Pencode2(H5P_DATASET_CREATE, nullptr, &encoded_size,
                         H5P_DEFAULT),
              0);
    EXPECT_LT(H5Pencode2(dcpl.get(), nullptr, nullptr, H5P_DEFAULT), 0);

    EXPECT_EQ(H5Pdecode(nullptr), H5I_INVALID_HID);

    std::vector<uint8_t> bad_vers = buf;
    bad_vers[0] = 99;
    EXPECT_EQ(H5Pdecode(bad_vers.data()), H5I_INVALID_HID);

    std::vector<uint8_t> bad_type = buf;
    bad_type[1] = 0;  // H5P_TYPE_USER cannot be decoded.
    EXPECT_EQ(H5Pdecode(bad_type.data()), H5I_INVALID_HID);
  }
}

TEST(H5PTest, StaleClassAndListIdsFailGenericApisCleanly) {
  ScopedPlist valid_plist(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(valid_plist.valid());

  hid_t stale_cls =
      H5Pcreate_class(H5P_ROOT, "h5p_stale_cls", nullptr, nullptr, nullptr,
                      nullptr, nullptr, nullptr);
  ASSERT_GE(stale_cls, 0);
  hid_t stale_plist = H5Pcreate(stale_cls);
  ASSERT_GE(stale_plist, 0);
  ASSERT_GE(H5Pclose(stale_plist), 0);
  ASSERT_GE(H5Pclose_class(stale_cls), 0);

  ScopedErrorSilence silence;
  size_t sz = 0;
  size_t nprops = 0;
  IterCollector collector;

  EXPECT_EQ(H5Pcopy(stale_plist), H5I_INVALID_HID);
  EXPECT_EQ(H5Pcopy(stale_cls), H5I_INVALID_HID);
  EXPECT_EQ(H5Pcreate_class(stale_cls, "stale_child", nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr),
            H5I_INVALID_HID);
  EXPECT_LT(H5Pexist(stale_plist, "p"), 0);
  EXPECT_LT(H5Pexist(stale_cls, "p"), 0);
  EXPECT_LT(H5Pget_size(stale_plist, "p", &sz), 0);
  EXPECT_LT(H5Pget_size(stale_cls, "p", &sz), 0);
  EXPECT_LT(H5Pget_nprops(stale_plist, &nprops), 0);
  EXPECT_LT(H5Pget_nprops(stale_cls, &nprops), 0);
  EXPECT_LT(H5Pequal(stale_plist, valid_plist.get()), 0);
  EXPECT_LT(H5Pequal(valid_plist.get(), stale_plist), 0);
  EXPECT_LT(H5Pequal(stale_cls, H5P_ROOT), 0);
  EXPECT_LT(H5Pisa_class(stale_plist, H5P_ROOT), 0);
  EXPECT_LT(H5Pisa_class(valid_plist.get(), stale_cls), 0);
  EXPECT_LT(H5Piterate(stale_plist, nullptr, CollectPropsIterCb, &collector),
            0);
  EXPECT_LT(H5Piterate(stale_cls, nullptr, CollectPropsIterCb, &collector), 0);
  EXPECT_LT(H5Pclose(stale_plist), 0);
  EXPECT_LT(H5Pclose_class(stale_cls), 0);
}

}  // namespace
}  // namespace hdf5
