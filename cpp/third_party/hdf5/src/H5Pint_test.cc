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
// Tests generic property list and property class internals: class hierarchy,
// copy-on-write class splitting, property lifecycle callbacks, peek/poke and
// set/get operations, deletion/reinsertion, comparison, iteration, type
// instantiation, and package initialization/termination
// (third_party/hdf5/src/H5Pint.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pint.c

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` compiles with -DNDEBUG. Match its struct layouts when accessing
// internal H5P types via H5Ppkg.h.
#ifndef NDEBUG
#define H5P_INT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5P_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Ppkg.h"
}  // extern "C"
#ifdef H5P_INT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5P_INT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

using ScopedPclass = ScopedH5Id<H5Pclose_class>;

struct CallbackTracker {
  std::vector<std::string> events;
  bool fail_cls_create = false;
  bool fail_cls_copy = false;
  bool fail_prp_create = false;
  bool fail_prp_set = false;
  bool fail_prp_get = false;
  bool fail_prp_del = false;
  bool fail_prp_copy = false;
  bool fail_prp_close = false;
  std::string fail_on_prop;
};

CallbackTracker*& ActiveTracker() {
  static CallbackTracker* tracker = nullptr;
  return tracker;
}

class ScopedTracker {
 public:
  explicit ScopedTracker(CallbackTracker* tracker) : prev_(ActiveTracker()) {
    ActiveTracker() = tracker;
  }
  ~ScopedTracker() { ActiveTracker() = prev_; }
  ScopedTracker(const ScopedTracker&) = delete;
  ScopedTracker& operator=(const ScopedTracker&) = delete;

 private:
  CallbackTracker* prev_;
};

herr_t ClsCreateCb(hid_t plist_id, void* data) {
  auto* tag = static_cast<const char*>(data);
  if (auto* tr = ActiveTracker()) {
    tr->events.push_back(std::string("cls_create:") + (tag ? tag : ""));
    if (tr->fail_cls_create) return -1;
  }
  (void)plist_id;
  return 0;
}

herr_t ClsCopyCb(hid_t new_id, hid_t old_id, void* data) {
  auto* tag = static_cast<const char*>(data);
  if (auto* tr = ActiveTracker()) {
    tr->events.push_back(std::string("cls_copy:") + (tag ? tag : ""));
    if (tr->fail_cls_copy) return -1;
  }
  (void)new_id;
  (void)old_id;
  return 0;
}

herr_t ClsCloseCb(hid_t plist_id, void* data) {
  auto* tag = static_cast<const char*>(data);
  if (auto* tr = ActiveTracker()) {
    tr->events.push_back(std::string("cls_close:") + (tag ? tag : ""));
  }
  (void)plist_id;
  return 0;
}

herr_t AltClsCreateCb(hid_t, void*) { return 0; }
herr_t AltClsCloseCb(hid_t, void*) { return 0; }

herr_t PrpCreateCb(const char* name, size_t size, void* value) {
  if (auto* tr = ActiveTracker()) {
    tr->events.push_back(std::string("prp_create:") + name);
    if (tr->fail_prp_create &&
        (tr->fail_on_prop.empty() || tr->fail_on_prop == name)) {
      return -1;
    }
  }
  if (size == sizeof(int) && value != nullptr) {
    int v = 0;
    std::memcpy(&v, value, sizeof(v));
    v += 1;
    std::memcpy(value, &v, sizeof(v));
  }
  return 0;
}

herr_t AltPrpCreateCb(const char*, size_t, void*) { return 0; }

herr_t PrpSetCb(hid_t, const char* name, size_t size, void* value) {
  if (auto* tr = ActiveTracker()) {
    tr->events.push_back(std::string("prp_set:") + name);
    if (tr->fail_prp_set) return -1;
  }
  if (size == sizeof(int) && value != nullptr) {
    int v = 0;
    std::memcpy(&v, value, sizeof(v));
    v += 10;
    std::memcpy(value, &v, sizeof(v));
  }
  return 0;
}

herr_t AltPrpSetCb(hid_t, const char*, size_t, void*) { return 0; }

herr_t PrpGetCb(hid_t, const char* name, size_t size, void* value) {
  if (auto* tr = ActiveTracker()) {
    tr->events.push_back(std::string("prp_get:") + name);
    if (tr->fail_prp_get) return -1;
  }
  if (size == sizeof(int) && value != nullptr) {
    int v = 0;
    std::memcpy(&v, value, sizeof(v));
    v += 100;
    std::memcpy(value, &v, sizeof(v));
  }
  return 0;
}

herr_t AltPrpGetCb(hid_t, const char*, size_t, void*) { return 0; }

herr_t PrpEncodeCb(const void*, void**, size_t*) { return 0; }
herr_t AltPrpEncodeCb(const void*, void**, size_t*) { return 0; }

herr_t PrpDecodeCb(const void**, void*) { return 0; }
herr_t AltPrpDecodeCb(const void**, void*) { return 0; }

herr_t PrpDelCb(hid_t, const char* name, size_t, void*) {
  if (auto* tr = ActiveTracker()) {
    tr->events.push_back(std::string("prp_del:") + name);
    if (tr->fail_prp_del) return -1;
  }
  return 0;
}

herr_t AltPrpDelCb(hid_t, const char*, size_t, void*) { return 0; }

herr_t PrpCopyCb(const char* name, size_t size, void* value) {
  if (auto* tr = ActiveTracker()) {
    tr->events.push_back(std::string("prp_copy:") + name);
    if (tr->fail_prp_copy) return -1;
  }
  if (size == sizeof(int) && value != nullptr) {
    int v = 0;
    std::memcpy(&v, value, sizeof(v));
    v += 2;
    std::memcpy(value, &v, sizeof(v));
  }
  return 0;
}

herr_t AltPrpCopyCb(const char*, size_t, void*) { return 0; }

int PrpCmpIntCb(const void* v1, const void* v2, size_t) {
  int a = 0;
  int b = 0;
  std::memcpy(&a, v1, sizeof(a));
  std::memcpy(&b, v2, sizeof(b));
  return (a > b) - (a < b);
}

int AltPrpCmpIntCb(const void* v1, const void* v2, size_t) {
  int a = 0;
  int b = 0;
  std::memcpy(&a, v1, sizeof(a));
  std::memcpy(&b, v2, sizeof(b));
  return (a > b) - (a < b);
}

herr_t PrpCloseCb(const char* name, size_t, void*) {
  if (auto* tr = ActiveTracker()) {
    tr->events.push_back(std::string("prp_close:") + name);
    if (tr->fail_prp_close) return -1;
  }
  return 0;
}

herr_t AltPrpCloseCb(const char*, size_t, void*) { return 0; }

TEST(H5PintTest, ClassHierarchyPathParentIsaAndOpenByPath) {
  ASSERT_GE(H5open(), 0);

  ScopedPclass parent_cls(H5Pcreate_class(H5P_ROOT, "h5pint_parent", nullptr,
                                          nullptr, nullptr, nullptr, nullptr,
                                          nullptr));
  ASSERT_TRUE(parent_cls.valid());

  ScopedPclass child_cls(H5Pcreate_class(parent_cls.get(), "h5pint_child",
                                         nullptr, nullptr, nullptr, nullptr,
                                         nullptr, nullptr));
  ASSERT_TRUE(child_cls.valid());

  ScopedPclass top_cls(H5Pcreate_class(H5P_DEFAULT, "h5pint_top_no_parent",
                                       nullptr, nullptr, nullptr, nullptr,
                                       nullptr, nullptr));
  ASSERT_TRUE(top_cls.valid());

  char* cname = H5Pget_class_name(child_cls.get());
  ASSERT_NE(cname, nullptr);
  absl::Cleanup free_cname = [cname] { H5free_memory(cname); };
  EXPECT_STREQ(cname, "h5pint_child");

  auto* child_obj = static_cast<H5P_genclass_t*>(
      H5I_object_verify(child_cls.get(), H5I_GENPROP_CLS));
  ASSERT_NE(child_obj, nullptr);
  char* full_path = H5P__get_class_path(child_obj);
  ASSERT_NE(full_path, nullptr);
  absl::Cleanup free_path = [full_path] { H5MM_xfree(full_path); };
  EXPECT_STREQ(full_path, "root/h5pint_parent/h5pint_child");

  auto* root_obj = static_cast<H5P_genclass_t*>(
      H5I_object_verify(H5P_ROOT, H5I_GENPROP_CLS));
  ASSERT_NE(root_obj, nullptr);
  char* root_path = H5P__get_class_path(root_obj);
  ASSERT_NE(root_path, nullptr);
  absl::Cleanup free_root_path = [root_path] { H5MM_xfree(root_path); };
  EXPECT_STREQ(root_path, "root");
  EXPECT_EQ(H5P__get_class_parent(root_obj), nullptr);

  ScopedPclass queried_parent(H5Pget_class_parent(child_cls.get()));
  ASSERT_TRUE(queried_parent.valid());
  EXPECT_GT(H5Pequal(queried_parent.get(), parent_cls.get()), 0);

  ScopedPlist child_plist(H5Pcreate(child_cls.get()));
  ASSERT_TRUE(child_plist.valid());
  EXPECT_GT(H5Pisa_class(child_plist.get(), child_cls.get()), 0);
  EXPECT_GT(H5Pisa_class(child_plist.get(), parent_cls.get()), 0);
  EXPECT_GT(H5Pisa_class(child_plist.get(), H5P_ROOT), 0);
  EXPECT_EQ(H5Pisa_class(child_plist.get(), top_cls.get()), 0);

  // Open existing classes via H5P__open_class_path (multi-part and single).
  H5P_genclass_t* opened_child = H5P__open_class_path(full_path);
  ASSERT_NE(opened_child, nullptr);
  absl::Cleanup close_opened_child = [opened_child] {
    H5P__close_class(opened_child);
  };
  EXPECT_STREQ(opened_child->name, "h5pint_child");

  H5P_genclass_t* opened_root = H5P__open_class_path("root");
  ASSERT_NE(opened_root, nullptr);
  absl::Cleanup close_opened_root = [opened_root] {
    H5P__close_class(opened_root);
  };
  EXPECT_STREQ(opened_root->name, "root");

  ScopedErrorSilence silence;
  EXPECT_EQ(H5P__open_class_path("no_such_root/h5pint_child"), nullptr);
  EXPECT_EQ(H5P__open_class_path("root/h5pint_parent/no_such_child"), nullptr);
  EXPECT_LT(H5P_isa_class(H5I_INVALID_HID, child_cls.get()), 0);
  EXPECT_LT(H5P_isa_class(child_plist.get(), H5I_INVALID_HID), 0);
}

TEST(H5PintTest, ClassRefCountResurrectionAndCascadingDestroy) {
  ASSERT_GE(H5open(), 0);

  ScopedPclass par(H5Pcreate_class(H5P_ROOT, "res_par", nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr));
  ASSERT_TRUE(par.valid());

  ScopedPclass child(H5Pcreate_class(par.get(), "res_child", nullptr, nullptr,
                                     nullptr, nullptr, nullptr, nullptr));
  ASSERT_TRUE(child.valid());

  ScopedPlist pl(H5Pcreate(child.get()));
  ASSERT_TRUE(pl.valid());

  // Close parent while child exists, and close child while plist exists;
  // child becomes marked `deleted` with `plists == 1`.
  par.reset();
  child.reset();

  // H5Pget_class() triggers H5P__access_class(H5P_MOD_INC_REF) on `deleted`
  // class, clearing `deleted = false` before creating a new class ID.
  ScopedPclass resurrected(H5Pget_class(pl.get()));
  ASSERT_TRUE(resurrected.valid());
  char* name = H5Pget_class_name(resurrected.get());
  ASSERT_NE(name, nullptr);
  absl::Cleanup free_name = [name] { H5free_memory(name); };
  EXPECT_STREQ(name, "res_child");

  // Releasing `resurrected` then `pl` cascades cleanup of child and parent.
  resurrected.reset();
  pl.reset();
}

TEST(H5PintTest, CopyClassAndCopyOnWriteClassSplitting) {
  ASSERT_GE(H5open(), 0);

  ScopedPclass empty_cls(H5Pcreate_class(H5P_ROOT, "empty_cls", nullptr,
                                         nullptr, nullptr, nullptr, nullptr,
                                         nullptr));
  ASSERT_TRUE(empty_cls.valid());
  ScopedPclass empty_copy(H5Pcopy(empty_cls.get()));
  ASSERT_TRUE(empty_copy.valid());
  EXPECT_GT(H5Pequal(empty_cls.get(), empty_copy.get()), 0);

  ScopedPclass cls(H5Pcreate_class(H5P_ROOT, "cow_cls", nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr));
  ASSERT_TRUE(cls.valid());

  int init_val = 42;
  ASSERT_GE(H5Pregister2(cls.get(), "p1", sizeof(init_val), &init_val, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
            0);

  ScopedPclass cls_copy(H5Pcopy(cls.get()));
  ASSERT_TRUE(cls_copy.valid());
  EXPECT_GT(H5Pequal(cls.get(), cls_copy.get()), 0);

  // Create a property list so `pclass->plists > 0`; subsequent registration
  // splits the class via copy-on-write and substitutes the new H5P_genclass_t.
  ScopedPlist pl_before(H5Pcreate(cls.get()));
  ASSERT_TRUE(pl_before.valid());

  int val2 = 99;
  ASSERT_GE(H5Pregister2(cls.get(), "p2", sizeof(val2), &val2, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
            0);
  EXPECT_EQ(H5Pexist(pl_before.get(), "p2"), 0);
  EXPECT_GT(H5Pexist(cls.get(), "p2"), 0);

  ScopedPlist pl_after(H5Pcreate(cls.get()));
  ASSERT_TRUE(pl_after.valid());
  EXPECT_GT(H5Pexist(pl_after.get(), "p2"), 0);

  // Registering a duplicate name while pl_after exists triggers error cleanup
  // of the freshly allocated split class inside H5P__register().
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Pregister2(cls.get(), "p2", sizeof(val2), &val2, nullptr, nullptr,
                     nullptr, nullptr, nullptr, nullptr, nullptr),
        0);
  }

  // Test H5Pcopy_prop() between classes, including split when target has plist.
  ScopedPclass src_cls(H5Pcreate_class(H5P_ROOT, "cow_src", nullptr, nullptr,
                                       nullptr, nullptr, nullptr, nullptr));
  ASSERT_TRUE(src_cls.valid());
  int src_val = 777;
  ASSERT_GE(H5Pregister2(src_cls.get(), "p_from_src", sizeof(src_val), &src_val,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr),
            0);
  ASSERT_GE(
      H5Pregister2(src_cls.get(), "p2", sizeof(src_val), &src_val, nullptr,
                   nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
      0);

  // Copy new property and overwrite existing property after class has a plist.
  ScopedPlist hold_pl(H5Pcreate(cls.get()));
  ASSERT_TRUE(hold_pl.valid());
  EXPECT_GE(H5Pcopy_prop(cls.get(), src_cls.get(), "p_from_src"), 0);
  EXPECT_GT(H5Pexist(cls.get(), "p_from_src"), 0);
  EXPECT_GE(H5Pcopy_prop(cls.get(), src_cls.get(), "p2"), 0);

  // H5Punregister removes property; removing missing property fails.
  EXPECT_GE(H5Punregister(cls.get(), "p_from_src"), 0);
  EXPECT_EQ(H5Pexist(cls.get(), "p_from_src"), 0);
  ScopedErrorSilence silence;
  EXPECT_LT(H5Punregister(cls.get(), "p_from_src"), 0);
  EXPECT_LT(H5Pcopy_prop(cls.get(), src_cls.get(), "missing"), 0);
}

TEST(H5PintTest, PropertyAndClassLifecycleCallbacksAndShadowing) {
  ASSERT_GE(H5open(), 0);
  CallbackTracker tracker;
  ScopedTracker guard(&tracker);

  char par_tag[] = "par";
  char child_tag[] = "child";
  ScopedPclass par_cls(H5Pcreate_class(H5P_ROOT, "cb_par", ClsCreateCb, par_tag,
                                       ClsCopyCb, par_tag, ClsCloseCb,
                                       par_tag));
  ASSERT_TRUE(par_cls.valid());

  int par_default = 10;
  ASSERT_GE(H5Pregister2(par_cls.get(), "shared", sizeof(par_default),
                         &par_default, PrpCreateCb, PrpSetCb, PrpGetCb,
                         PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb),
            0);
  ASSERT_GE(H5Pregister2(par_cls.get(), "par_only", sizeof(par_default),
                         &par_default, PrpCreateCb, nullptr, nullptr, nullptr,
                         PrpCopyCb, nullptr, PrpCloseCb),
            0);

  ScopedPclass child_cls(H5Pcreate_class(par_cls.get(), "cb_child", ClsCreateCb,
                                         child_tag, ClsCopyCb, child_tag,
                                         ClsCloseCb, child_tag));
  ASSERT_TRUE(child_cls.valid());

  // Register a property with the same name as parent's "shared" to test
  // shadowing + `seen` skip-list deduplication in create/copy/close/iterate.
  int child_default = 50;
  ASSERT_GE(H5Pregister2(child_cls.get(), "shared", sizeof(child_default),
                         &child_default, PrpCreateCb, PrpSetCb, PrpGetCb,
                         PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb),
            0);

  ScopedPlist pl(H5Pcreate(child_cls.get()));
  ASSERT_TRUE(pl.valid());
  EXPECT_THAT(tracker.events,
              ElementsAre("prp_create:shared", "prp_create:par_only",
                          "cls_create:child", "cls_create:par"));

  int val = 0;
  ASSERT_GE(H5Pget(pl.get(), "shared", &val), 0);
  // 50 + 1 (create) + 100 (get) = 151.
  EXPECT_EQ(val, 151);

  tracker.events.clear();
  ScopedPlist pl_copy(H5Pcopy(pl.get()));
  ASSERT_TRUE(pl_copy.valid());
  // Note: H5P_copy_plist passes old_plist->pclass->copy_data to each ancestor
  // copy_func in the hierarchy (so both callbacks receive child_tag here).
  EXPECT_THAT(tracker.events,
              ElementsAre("prp_copy:par_only", "prp_copy:shared",
                          "cls_copy:child", "cls_copy:child"));

  tracker.events.clear();
  pl_copy.reset();
  EXPECT_THAT(tracker.events,
              ElementsAre("cls_close:child", "cls_close:par",
                          "prp_close:par_only", "prp_close:shared"));
}

TEST(H5PintTest, CallbackFailurePathsAndIncompleteClassInitCmp) {
  ASSERT_GE(H5open(), 0);
  CallbackTracker tracker;
  ScopedTracker guard(&tracker);

  // Class create_func failing should clean up via H5P_close with
  // class_init == false (skipping class close_func), and we can also test
  // H5P__cmp_plist class_init mismatch while inside create_func.
  struct CmpState {
    hid_t completed_plist = H5I_INVALID_HID;
    htri_t cmp_new_vs_old = -1;
    htri_t cmp_old_vs_new = -1;
    bool should_fail = false;
  } cmp_state;

  auto create_fn = [](hid_t new_id, void* data) -> herr_t {
    auto* st = static_cast<CmpState*>(data);
    if (st->completed_plist != H5I_INVALID_HID) {
      st->cmp_new_vs_old = H5Pequal(new_id, st->completed_plist);
      st->cmp_old_vs_new = H5Pequal(st->completed_plist, new_id);
    }
    return st->should_fail ? -1 : 0;
  };
  auto copy_fn = [](hid_t, hid_t, void* data) -> herr_t {
    auto* st = static_cast<CmpState*>(data);
    return st->should_fail ? -1 : 0;
  };

  ScopedPclass cls(H5Pcreate_class(H5P_ROOT, "fail_cls", create_fn, &cmp_state,
                                   copy_fn, &cmp_state, ClsCloseCb, nullptr));
  ASSERT_TRUE(cls.valid());

  int d1 = 1;
  int d2 = 2;
  ASSERT_GE(
      H5Pregister2(cls.get(), "a_prop", sizeof(d1), &d1, PrpCreateCb, nullptr,
                   nullptr, nullptr, PrpCopyCb, nullptr, PrpCloseCb),
      0);
  ASSERT_GE(
      H5Pregister2(cls.get(), "b_prop", sizeof(d2), &d2, PrpCreateCb, nullptr,
                   nullptr, nullptr, PrpCopyCb, nullptr, PrpCloseCb),
      0);

  // First create succeeds to serve as completed_plist.
  ScopedPlist pl_done(H5Pcreate(cls.get()));
  ASSERT_TRUE(pl_done.valid());
  cmp_state.completed_plist = pl_done.get();

  // Second create runs H5Pequal inside create_func while class_init == false.
  ScopedPlist pl2(H5Pcreate(cls.get()));
  ASSERT_TRUE(pl2.valid());
  EXPECT_EQ(cmp_state.cmp_new_vs_old, 0);
  EXPECT_EQ(cmp_state.cmp_old_vs_new, 0);

  // 1. Class create callback fails -> H5P_create_id removes ID and calls
  // H5P_close with class_init == false (so ClsCloseCb is NOT invoked).
  {
    ScopedErrorSilence silence;
    tracker.events.clear();
    cmp_state.should_fail = true;
    EXPECT_EQ(H5Pcreate(cls.get()), H5I_INVALID_HID);
    EXPECT_THAT(tracker.events,
                ElementsAre("prp_create:a_prop", "prp_create:b_prop",
                            "prp_close:a_prop", "prp_close:b_prop"));
  }

  // 2. Class copy callback fails -> H5P_copy_plist cleans up new plist.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Pcopy(pl_done.get()), H5I_INVALID_HID);
  }
  cmp_state.should_fail = false;

  // 3. Property create callback fails on second property -> H5P__create frees
  // already-created first property using make_cb = 1 (calls prp_close).
  {
    ScopedErrorSilence silence;
    tracker.events.clear();
    tracker.fail_prp_create = true;
    tracker.fail_on_prop = "b_prop";
    EXPECT_EQ(H5Pcreate(cls.get()), H5I_INVALID_HID);
    EXPECT_THAT(tracker.events,
                ElementsAre("prp_create:a_prop", "prp_create:b_prop",
                            "prp_close:a_prop"));
    tracker.fail_prp_create = false;
    tracker.fail_on_prop.clear();
  }
}

TEST(H5PintTest, SetGetPokePeekZeroSizeAndClassGetSet) {
  ASSERT_GE(H5open(), 0);
  CallbackTracker tracker;
  ScopedTracker guard(&tracker);

  ScopedPclass cls(H5Pcreate_class(H5P_ROOT, "sgpp_cls", nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr));
  ASSERT_TRUE(cls.valid());

  int def_v = 5;
  ASSERT_GE(
      H5Pregister2(cls.get(), "num", sizeof(def_v), &def_v, nullptr, PrpSetCb,
                   PrpGetCb, PrpDelCb, nullptr, nullptr, nullptr),
      0);
  ASSERT_GE(H5Pregister2(cls.get(), "flag", 0, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
            0);

  // Test H5P__class_get and H5P__class_set directly on H5P_genclass_t.
  auto* gen_cls = static_cast<H5P_genclass_t*>(
      H5I_object_verify(cls.get(), H5I_GENPROP_CLS));
  ASSERT_NE(gen_cls, nullptr);
  int cls_val = 0;
  ASSERT_GE(H5P__class_get(gen_cls, "num", &cls_val), 0);
  EXPECT_EQ(cls_val, 5);
  cls_val = 12;
  ASSERT_GE(H5P__class_set(gen_cls, "num", &cls_val), 0);
  int cls_val_read = 0;
  ASSERT_GE(H5P__class_get(gen_cls, "num", &cls_val_read), 0);
  EXPECT_EQ(cls_val_read, 12);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5P__class_get(gen_cls, "missing", &cls_val_read), 0);
    EXPECT_LT(H5P__class_get(gen_cls, "flag", &cls_val_read), 0);
    EXPECT_LT(H5P__class_set(gen_cls, "missing", &cls_val), 0);
    EXPECT_LT(H5P__class_set(gen_cls, "flag", &cls_val), 0);
  }

  ScopedPlist pl(H5Pcreate(cls.get()));
  ASSERT_TRUE(pl.valid());
  auto* plist = static_cast<H5P_genplist_t*>(
      H5I_object_verify(pl.get(), H5I_GENPROP_LST));
  ASSERT_NE(plist, nullptr);

  // Peek on unmodified class property does NOT invoke PrpGetCb.
  int out = 0;
  ASSERT_GE(H5P_peek(plist, "num", &out), 0);
  EXPECT_EQ(out, 12);
  EXPECT_THAT(tracker.events, IsEmpty());

  // Get on unmodified class property invokes PrpGetCb (+100).
  ASSERT_GE(H5Pget(pl.get(), "num", &out), 0);
  EXPECT_EQ(out, 112);
  EXPECT_THAT(tracker.events, ElementsAre("prp_get:num"));

  // Set on unmodified class property (H5P__set_pclass_cb) invokes PrpSetCb
  // (+10).
  tracker.events.clear();
  int set_v = 20;
  ASSERT_GE(H5Pset(pl.get(), "num", &set_v), 0);
  EXPECT_THAT(tracker.events, ElementsAre("prp_set:num"));
  ASSERT_GE(H5P_peek(plist, "num", &out), 0);
  EXPECT_EQ(out, 30);

  // Set on already-modified list property (H5P__set_plist_cb) invokes del &
  // set.
  tracker.events.clear();
  set_v = 50;
  ASSERT_GE(H5Pset(pl.get(), "num", &set_v), 0);
  EXPECT_THAT(tracker.events, ElementsAre("prp_set:num", "prp_del:num"));
  ASSERT_GE(H5P_peek(plist, "num", &out), 0);
  EXPECT_EQ(out, 60);

  // Poke on modified list property does NOT invoke set or del callbacks.
  tracker.events.clear();
  set_v = 77;
  ASSERT_GE(H5P_poke(plist, "num", &set_v), 0);
  EXPECT_THAT(tracker.events, IsEmpty());
  ASSERT_GE(H5P_peek(plist, "num", &out), 0);
  EXPECT_EQ(out, 77);

  // Poke on unmodified class property (H5P__poke_pclass_cb).
  ScopedPlist pl_fresh(H5Pcreate(cls.get()));
  ASSERT_TRUE(pl_fresh.valid());
  auto* plist_fresh = static_cast<H5P_genplist_t*>(
      H5I_object_verify(pl_fresh.get(), H5I_GENPROP_LST));
  ASSERT_NE(plist_fresh, nullptr);
  int poke_v = 88;
  ASSERT_GE(H5P_poke(plist_fresh, "num", &poke_v), 0);
  ASSERT_GE(H5P_peek(plist_fresh, "num", &out), 0);
  EXPECT_EQ(out, 88);

  // Insert zero-size and sized list-local properties.
  ASSERT_GE(H5Pinsert2(pl.get(), "local_flag", 0, nullptr, nullptr, nullptr,
                       PrpDelCb, nullptr, nullptr, PrpCloseCb),
            0);
  int local_val = 33;
  ASSERT_GE(H5Pinsert2(pl.get(), "local_num", sizeof(local_val), &local_val,
                       nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
            0);
  int got_local = 0;
  ASSERT_GE(H5Pget(pl.get(), "local_num", &got_local), 0);
  EXPECT_EQ(got_local, 33);
  int new_local = 44;
  ASSERT_GE(H5Pset(pl.get(), "local_num", &new_local), 0);
  ASSERT_GE(H5Pget(pl.get(), "local_num", &got_local), 0);
  EXPECT_EQ(got_local, 44);

  // Copying a list duplicates non-shared list-inserted property names.
  ScopedPlist pl_copy_local(H5Pcopy(pl.get()));
  ASSERT_TRUE(pl_copy_local.valid());
  int copy_local_val = 0;
  ASSERT_GE(H5Pget(pl_copy_local.get(), "local_num", &copy_local_val), 0);
  EXPECT_EQ(copy_local_val, 44);

  // Negative callback failures and zero-size restrictions.
  {
    ScopedErrorSilence silence;
    tracker.fail_prp_set = true;
    EXPECT_LT(H5Pset(pl.get(), "num", &set_v), 0);
    EXPECT_LT(H5Pset(pl_fresh.get(), "num", &set_v), 0);
    ScopedPlist pl_unmod(H5Pcreate(cls.get()));
    ASSERT_TRUE(pl_unmod.valid());
    EXPECT_LT(H5Pset(pl_unmod.get(), "num", &set_v), 0);
    tracker.fail_prp_set = false;

    tracker.fail_prp_del = true;
    EXPECT_LT(H5Pset(pl.get(), "num", &set_v), 0);
    tracker.fail_prp_del = false;

    tracker.fail_prp_get = true;
    EXPECT_LT(H5Pget(pl.get(), "num", &out), 0);
    tracker.fail_prp_get = false;

    // Zero-size property cannot be set/get/poked/peeked.
    EXPECT_LT(H5Pset(pl.get(), "flag", &set_v), 0);
    EXPECT_LT(H5Pget(pl.get(), "flag", &out), 0);
    EXPECT_LT(H5P_poke(plist, "flag", &set_v), 0);
    EXPECT_LT(H5P_peek(plist, "flag", &out), 0);
    EXPECT_LT(H5Pset(pl.get(), "local_flag", &set_v), 0);
    EXPECT_LT(H5Pget(pl.get(), "local_flag", &out), 0);
    EXPECT_LT(H5P_poke(plist, "local_flag", &set_v), 0);
    EXPECT_LT(H5P_peek(plist, "local_flag", &out), 0);

    // Missing property errors.
    EXPECT_LT(H5Pset(pl.get(), "absent", &set_v), 0);
    EXPECT_LT(H5Pget(pl.get(), "absent", &out), 0);
    EXPECT_LT(H5P_poke(plist, "absent", &set_v), 0);
    EXPECT_LT(H5P_peek(plist, "absent", &out), 0);
  }
}

TEST(H5PintTest, RemoveReinsertCopyPropAndExistSizeNprops) {
  ASSERT_GE(H5open(), 0);
  CallbackTracker tracker;
  ScopedTracker guard(&tracker);

  ScopedPclass par(H5Pcreate_class(H5P_ROOT, "rm_par", nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr));
  ASSERT_TRUE(par.valid());
  int v = 1;
  ASSERT_GE(H5Pregister2(par.get(), "par_prop", sizeof(v), &v, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
            0);

  ScopedPclass cls(H5Pcreate_class(par.get(), "rm_cls", nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr));
  ASSERT_TRUE(cls.valid());
  ASSERT_GE(
      H5Pregister2(cls.get(), "cls_prop", sizeof(v), &v, PrpCreateCb, nullptr,
                   nullptr, PrpDelCb, PrpCopyCb, nullptr, PrpCloseCb),
      0);
  ASSERT_GE(H5Pregister2(cls.get(), "plain_prop", sizeof(v), &v, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
            0);

  ScopedPlist pl(H5Pcreate(cls.get()));
  ASSERT_TRUE(pl.valid());
  size_t nprops = 0;
  ASSERT_GE(H5Pget_nprops(pl.get(), &nprops), 0);
  EXPECT_EQ(nprops, 3);
  ASSERT_GE(H5Pget_nprops(cls.get(), &nprops), 0);
  EXPECT_EQ(nprops, 2);
  auto* gen_cls = static_cast<H5P_genclass_t*>(
      H5I_object_verify(cls.get(), H5I_GENPROP_CLS));
  ASSERT_NE(gen_cls, nullptr);
  ASSERT_GE(H5P_get_nprops_pclass(gen_cls, &nprops, /*recurse=*/true), 0);
  EXPECT_EQ(nprops, 3);

  // Inserting duplicate of parent/current class prop fails.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pinsert2(pl.get(), "par_prop", sizeof(v), &v, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pinsert2(pl.get(), "cls_prop", sizeof(v), &v, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr),
              0);
  }

  // Delete failure on list prop (cls_prop is in pl->props because of
  // PrpCreateCb) and on class prop (plain_prop is in class, or insert/test del
  // failure).
  {
    ScopedErrorSilence silence;
    tracker.fail_prp_del = true;
    EXPECT_LT(H5Premove(pl.get(), "cls_prop"), 0);
    tracker.fail_prp_del = false;
  }

  // Remove list prop and unmodified class prop cleanly.
  EXPECT_GE(H5Premove(pl.get(), "cls_prop"), 0);
  EXPECT_GE(H5Premove(pl.get(), "plain_prop"), 0);
  EXPECT_EQ(H5Pexist(pl.get(), "cls_prop"), 0);
  EXPECT_EQ(H5Pexist(pl.get(), "plain_prop"), 0);
  EXPECT_GT(H5Pexist(pl.get(), "par_prop"), 0);
  EXPECT_GT(H5Pexist(cls.get(), "cls_prop"), 0);
  EXPECT_GT(H5Pexist(cls.get(), "par_prop"), 0);
  EXPECT_EQ(H5Pexist(cls.get(), "nonexistent"), 0);

  size_t psize = 0;
  EXPECT_GE(H5Pget_size(pl.get(), "par_prop", &psize), 0);
  EXPECT_EQ(psize, sizeof(int));
  EXPECT_GE(H5Pget_size(cls.get(), "cls_prop", &psize), 0);
  EXPECT_EQ(psize, sizeof(int));
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pget_size(pl.get(), "cls_prop", &psize), 0);
    EXPECT_LT(H5Pget_size(cls.get(), "missing", &psize), 0);
    EXPECT_LT(H5Premove(pl.get(), "cls_prop"), 0);
  }

  // Copy plist containing deleted properties (`old_plist->del` path).
  ScopedPlist pl_with_del(H5Pcopy(pl.get()));
  ASSERT_TRUE(pl_with_del.valid());
  EXPECT_EQ(H5Pexist(pl_with_del.get(), "cls_prop"), 0);

  // Re-inserting a deleted property removes it from `del` and adds to `props`.
  int reinserted = 99;
  ASSERT_GE(H5Pinsert2(pl.get(), "cls_prop", sizeof(reinserted), &reinserted,
                       nullptr, nullptr, PrpDelCb, PrpCopyCb, nullptr, nullptr),
            0);
  EXPECT_GT(H5Pexist(pl.get(), "cls_prop"), 0);
  {
    ScopedErrorSilence silence;
    // Re-inserting an already inserted property fails.
    EXPECT_LT(H5Pinsert2(pl.get(), "cls_prop", sizeof(reinserted), &reinserted,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
  }

  // Remove unmodified class prop that has a del callback -> hits
  // H5P__del_pclass_cb tmp_value copy + del callback (both failing and
  // succeeding).
  ScopedPclass del_cls(H5Pcreate_class(H5P_ROOT, "del_cls", nullptr, nullptr,
                                       nullptr, nullptr, nullptr, nullptr));
  ASSERT_TRUE(del_cls.valid());
  ASSERT_GE(H5Pregister2(del_cls.get(), "dprop", sizeof(v), &v, nullptr,
                         nullptr, nullptr, PrpDelCb, nullptr, nullptr, nullptr),
            0);
  ScopedPlist del_pl(H5Pcreate(del_cls.get()));
  ASSERT_TRUE(del_pl.valid());
  {
    ScopedErrorSilence silence;
    tracker.fail_prp_del = true;
    EXPECT_LT(H5Premove(del_pl.get(), "dprop"), 0);
    tracker.fail_prp_del = false;
  }
  EXPECT_GE(H5Premove(del_pl.get(), "dprop"), 0);

  // H5Pcopy_prop on plists: overwrite existing + insert new + error cases.
  ScopedPlist src_pl(H5Pcreate(cls.get()));
  ASSERT_TRUE(src_pl.valid());
  int src_new = 55;
  ASSERT_GE(H5Pinsert2(src_pl.get(), "src_only", sizeof(src_new), &src_new,
                       nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
            0);
  // Overwrite existing `cls_prop` (removes dst then copies with PrpCopyCb).
  EXPECT_GE(H5Pcopy_prop(pl.get(), src_pl.get(), "cls_prop"), 0);
  // Copy missing `src_only` (creates without existing dst entry).
  EXPECT_GE(H5Pcopy_prop(pl.get(), src_pl.get(), "src_only"), 0);
  EXPECT_GT(H5Pexist(pl.get(), "src_only"), 0);
  // Copy `plain_prop` back into `pl` (was in `pl->del`; H5P__copy_prop_plist
  // inserts into `props` while `del` still shadows `H5Pexist`). A second copy
  // hits duplicate key failure in H5P__add_prop because `del` still shadows it.
  EXPECT_GE(H5Pcopy_prop(pl.get(), src_pl.get(), "plain_prop"), 0);
  EXPECT_EQ(H5Pexist(pl.get(), "plain_prop"), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pcopy_prop(pl.get(), src_pl.get(), "plain_prop"), 0);
  }

  // Also test copy_prop when `create` callback exists on missing dst prop.
  ScopedPclass copy_dst_cls(H5Pcreate_class(H5P_ROOT, "copy_dst_cls", nullptr,
                                            nullptr, nullptr, nullptr, nullptr,
                                            nullptr));
  ASSERT_TRUE(copy_dst_cls.valid());
  ScopedPlist copy_dst_pl(H5Pcreate(copy_dst_cls.get()));
  ASSERT_TRUE(copy_dst_pl.valid());
  EXPECT_GE(H5Pcopy_prop(copy_dst_pl.get(), src_pl.get(), "cls_prop"), 0);

  // Negative H5Pcopy_prop paths on plists and pclasses.
  ScopedPlist pl_overwrite(H5Pcreate(cls.get()));
  ASSERT_TRUE(pl_overwrite.valid());
  ScopedPclass src_with_par(H5Pcreate_class(H5P_ROOT, "src_with_par", nullptr,
                                            nullptr, nullptr, nullptr, nullptr,
                                            nullptr));
  ASSERT_TRUE(src_with_par.valid());
  ASSERT_GE(H5Pregister2(src_with_par.get(), "par_prop", sizeof(v), &v, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
            0);
  {
    ScopedErrorSilence silence;
    tracker.fail_prp_del = true;
    EXPECT_LT(H5Pcopy_prop(pl_overwrite.get(), src_pl.get(), "cls_prop"), 0);
    tracker.fail_prp_del = false;

    tracker.fail_prp_copy = true;
    EXPECT_LT(H5Pcopy_prop(pl_overwrite.get(), src_pl.get(), "cls_prop"), 0);
    tracker.fail_prp_copy = false;

    tracker.fail_prp_create = true;
    EXPECT_LT(H5Pcopy_prop(del_pl.get(), src_pl.get(), "cls_prop"), 0);
    tracker.fail_prp_create = false;

    // Overwriting a property inherited from a parent class fails to unregister
    // it from the derived class.
    EXPECT_LT(H5Pcopy_prop(cls.get(), src_with_par.get(), "par_prop"), 0);

    EXPECT_LT(H5Pcopy_prop(pl.get(), src_pl.get(), "not_in_src"), 0);
    EXPECT_LT(H5P__copy_prop_plist(H5I_INVALID_HID, src_pl.get(), "cls_prop"),
              0);
    EXPECT_LT(H5P__copy_prop_plist(pl.get(), H5I_INVALID_HID, "cls_prop"), 0);
    EXPECT_LT(H5P__copy_prop_pclass(H5I_INVALID_HID, cls.get(), "cls_prop"), 0);
    EXPECT_LT(H5P__copy_prop_pclass(cls.get(), H5I_INVALID_HID, "cls_prop"), 0);
  }
}

TEST(H5PintTest, ComparePropsClassesAndPlistsExhaustive) {
  ASSERT_GE(H5open(), 0);

  // Helper to compare two single-property classes differing in one aspect.
  auto make_prop_cls =
      [](const char* cls_name, const char* prop_name, size_t sz,
         const void* val, H5P_prp_create_func_t pcreate,
         H5P_prp_set_func_t pset, H5P_prp_get_func_t pget,
         H5P_prp_encode_func_t penc, H5P_prp_decode_func_t pdec,
         H5P_prp_delete_func_t pdel, H5P_prp_copy_func_t pcopy,
         H5P_prp_compare_func_t pcmp,
         H5P_prp_close_func_t pclose) -> H5P_genclass_t* {
    H5P_genclass_t* root = static_cast<H5P_genclass_t*>(
        H5I_object_verify(H5P_ROOT, H5I_GENPROP_CLS));
    H5P_genclass_t* c =
        H5P__create_class(root, cls_name, H5P_TYPE_USER, nullptr, nullptr,
                          nullptr, nullptr, nullptr, nullptr);
    if (c && prop_name) {
      H5P__register_real(c, prop_name, sz, val, pcreate, pset, pget, penc, pdec,
                         pdel, pcopy, pcmp, pclose);
    }
    return c;
  };

  int v1 = 10;
  int v2 = 20;
  H5P_genclass_t* base = make_prop_cls(
      "same", "p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb,
      PrpEncodeCb, PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb);
  ASSERT_NE(base, nullptr);
  absl::Cleanup close_base = [base] { H5P__close_class(base); };

  // Compare identical class against itself (fast revision check).
  EXPECT_EQ(H5P__cmp_class(base, base), 0);

  // Identical layout, different revisions -> compares all fields -> equal (0).
  H5P_genclass_t* clone = make_prop_cls(
      "same", "p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb,
      PrpEncodeCb, PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb);
  ASSERT_NE(clone, nullptr);
  absl::Cleanup close_clone = [clone] { H5P__close_class(clone); };
  EXPECT_EQ(H5P__cmp_class(base, clone), 0);

  // Test each difference branch inside H5P__cmp_prop via H5P__cmp_class.
  struct PropDiff {
    const char* name;
    size_t sz;
    const void* val;
    H5P_prp_create_func_t pcreate;
    H5P_prp_set_func_t pset;
    H5P_prp_get_func_t pget;
    H5P_prp_encode_func_t penc;
    H5P_prp_decode_func_t pdec;
    H5P_prp_delete_func_t pdel;
    H5P_prp_copy_func_t pcopy;
    H5P_prp_compare_func_t pcmp;
    H5P_prp_close_func_t pclose;
  };
  uint64_t big_val = 10;
  const PropDiff diffs[] = {
      {"q", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", 0, nullptr, PrpCreateCb, PrpSetCb, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(big_val), &big_val, PrpCreateCb, PrpSetCb, PrpGetCb,
       PrpEncodeCb, PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, nullptr, PrpSetCb, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, AltPrpCreateCb, PrpSetCb, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, nullptr, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, AltPrpSetCb, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, nullptr, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, AltPrpGetCb, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb, nullptr,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb, AltPrpEncodeCb,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb, PrpEncodeCb,
       nullptr, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb, PrpEncodeCb,
       AltPrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, nullptr, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, AltPrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, nullptr, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, AltPrpCopyCb, PrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, nullptr, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, AltPrpCmpIntCb, PrpCloseCb},
      {"p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, nullptr},
      {"p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, AltPrpCloseCb},
      {"p", sizeof(v2), &v2, PrpCreateCb, PrpSetCb, PrpGetCb, PrpEncodeCb,
       PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb},
  };
  for (const auto& d : diffs) {
    H5P_genclass_t* other =
        make_prop_cls("same", d.name, d.sz, d.val, d.pcreate, d.pset, d.pget,
                      d.penc, d.pdec, d.pdel, d.pcopy, d.pcmp, d.pclose);
    ASSERT_NE(other, nullptr);
    absl::Cleanup close_other = [other] { H5P__close_class(other); };
    EXPECT_NE(H5P__cmp_class(base, other), 0);
    EXPECT_NE(H5P__cmp_class(other, base), 0);
  }

  // Test prop->cmp == NULL vs non-NULL and prop->value == NULL vs non-NULL
  // branches in H5P__cmp_prop().
  {
    H5P_genclass_t* no_cmp_cls = make_prop_cls(
        "same", "p", sizeof(v1), &v1, PrpCreateCb, PrpSetCb, PrpGetCb,
        PrpEncodeCb, PrpDecodeCb, PrpDelCb, PrpCopyCb, PrpCmpIntCb, PrpCloseCb);
    ASSERT_NE(no_cmp_cls, nullptr);
    absl::Cleanup close_no_cmp = [no_cmp_cls] { H5P__close_class(no_cmp_cls); };
    auto* p =
        static_cast<H5P_genprop_t*>(H5SL_item(H5SL_first(no_cmp_cls->props)));
    ASSERT_NE(p, nullptr);
    H5P_prp_compare_func_t saved_cmp = p->cmp;
    p->cmp = nullptr;
    EXPECT_GT(H5P__cmp_class(base, no_cmp_cls), 0);
    EXPECT_LT(H5P__cmp_class(no_cmp_cls, base), 0);
    p->cmp = saved_cmp;

    H5P_genclass_t* z_null =
        make_prop_cls("zcmp", "z", 0, nullptr, nullptr, nullptr, nullptr,
                      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    H5P_genclass_t* z_val =
        make_prop_cls("zcmp", "z", 0, nullptr, nullptr, nullptr, nullptr,
                      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(z_null, nullptr);
    ASSERT_NE(z_val, nullptr);
    absl::Cleanup close_z_null = [z_null] { H5P__close_class(z_null); };
    absl::Cleanup close_z_val = [z_val] { H5P__close_class(z_val); };
    auto* zp = static_cast<H5P_genprop_t*>(H5SL_item(H5SL_first(z_val->props)));
    ASSERT_NE(zp, nullptr);
    zp->value = &v1;
    absl::Cleanup restore_zp = [zp] { zp->value = nullptr; };
    EXPECT_LT(H5P__cmp_class(z_null, z_val), 0);
    EXPECT_GT(H5P__cmp_class(z_val, z_null), 0);
  }

  // Test H5P__cmp_class class-metadata comparison branches.
  int tag_a = 1;
  int tag_b = 2;
  H5P_genclass_t* c1 =
      H5P__create_class(nullptr, "cmp_meta", H5P_TYPE_USER, ClsCreateCb, &tag_a,
                        nullptr, nullptr, ClsCloseCb, &tag_a);
  H5P_genclass_t* c2 =
      H5P__create_class(nullptr, "cmp_meta", H5P_TYPE_USER, ClsCreateCb, &tag_a,
                        nullptr, nullptr, ClsCloseCb, &tag_a);
  ASSERT_NE(c1, nullptr);
  ASSERT_NE(c2, nullptr);
  absl::Cleanup close_c1 = [c1] { H5P__close_class(c1); };
  absl::Cleanup close_c2 = [c2] { H5P__close_class(c2); };
  EXPECT_EQ(H5P__cmp_class(c1, c2), 0);

  // Different name, nprops, plists, classes, ref_count, type, deleted,
  // create_func/create_data, close_func/close_data.
  H5P_genclass_t* c_name =
      H5P__create_class(nullptr, "cmp_other", H5P_TYPE_USER, ClsCreateCb,
                        &tag_a, nullptr, nullptr, ClsCloseCb, &tag_a);
  ASSERT_NE(c_name, nullptr);
  absl::Cleanup close_c_name = [c_name] { H5P__close_class(c_name); };
  EXPECT_NE(H5P__cmp_class(c1, c_name), 0);

  H5P_genclass_t* c_nprops =
      H5P__create_class(nullptr, "cmp_meta", H5P_TYPE_USER, ClsCreateCb, &tag_a,
                        nullptr, nullptr, ClsCloseCb, &tag_a);
  ASSERT_NE(c_nprops, nullptr);
  absl::Cleanup close_c_nprops = [c_nprops] { H5P__close_class(c_nprops); };
  ASSERT_GE(H5P__register_real(c_nprops, "x", sizeof(v1), &v1, nullptr, nullptr,
                               nullptr, nullptr, nullptr, nullptr, nullptr,
                               nullptr, nullptr),
            0);
  EXPECT_LT(H5P__cmp_class(c1, c_nprops), 0);
  EXPECT_GT(H5P__cmp_class(c_nprops, c1), 0);

  // Different `plists` count.
  hid_t pl_id = H5P_create_id(c2, true);
  ASSERT_NE(pl_id, H5I_INVALID_HID);
  ScopedPlist pl_c2(pl_id);
  EXPECT_LT(H5P__cmp_class(c1, c2), 0);
  EXPECT_GT(H5P__cmp_class(c2, c1), 0);
  pl_c2.reset();

  // Different `classes` count.
  H5P_genclass_t* derived =
      H5P__create_class(c2, "derived", H5P_TYPE_USER, nullptr, nullptr, nullptr,
                        nullptr, nullptr, nullptr);
  ASSERT_NE(derived, nullptr);
  EXPECT_LT(H5P__cmp_class(c1, c2), 0);
  EXPECT_GT(H5P__cmp_class(c2, c1), 0);
  H5P__close_class(derived);

  // Different `ref_count`.
  ASSERT_GE(H5P__access_class(c2, H5P_MOD_INC_REF), 0);
  EXPECT_LT(H5P__cmp_class(c1, c2), 0);
  EXPECT_GT(H5P__cmp_class(c2, c1), 0);
  ASSERT_GE(H5P__access_class(c2, H5P_MOD_DEC_REF), 0);

  // Different `type`.
  H5P_genclass_t* c_type = H5P__create_class(
      nullptr, "cmp_meta", H5P_TYPE_OBJECT_CREATE, ClsCreateCb, &tag_a, nullptr,
      nullptr, ClsCloseCb, &tag_a);
  ASSERT_NE(c_type, nullptr);
  absl::Cleanup close_c_type = [c_type] { H5P__close_class(c_type); };
  EXPECT_LT(H5P__cmp_class(c1, c_type), 0);
  EXPECT_GT(H5P__cmp_class(c_type, c1), 0);

  // Different `deleted` (keep c_del alive via plist while decrementing ref).
  H5P_genclass_t* c_del =
      H5P__create_class(nullptr, "cmp_meta", H5P_TYPE_USER, ClsCreateCb, &tag_a,
                        nullptr, nullptr, ClsCloseCb, &tag_a);
  ASSERT_NE(c_del, nullptr);
  ScopedPlist hold_del(H5P_create_id(c_del, true));
  ASSERT_TRUE(hold_del.valid());
  ASSERT_GE(H5P__close_class(c_del), 0);  // ref_count=0 -> deleted=true.
  // Align plists and ref_count with c1 so comparison reaches the `deleted`
  // check, then restore both before releasing hold_del.
  c_del->plists = 0;
  c_del->ref_count = 1;
  EXPECT_LT(H5P__cmp_class(c1, c_del), 0);
  EXPECT_GT(H5P__cmp_class(c_del, c1), 0);
  c_del->ref_count = 0;
  c_del->plists = 1;
  hold_del.reset();

  // Different create_func / create_data / close_func / close_data.
  struct ClsDiff {
    H5P_cls_create_func_t cf;
    void* cd;
    H5P_cls_close_func_t clf;
    void* cld;
  } cls_diffs[] = {
      {nullptr, nullptr, ClsCloseCb, &tag_a},
      {AltClsCreateCb, &tag_a, ClsCloseCb, &tag_a},
      {ClsCreateCb, &tag_b, ClsCloseCb, &tag_a},
      {ClsCreateCb, &tag_a, nullptr, nullptr},
      {ClsCreateCb, &tag_a, AltClsCloseCb, &tag_a},
      {ClsCreateCb, &tag_a, ClsCloseCb, &tag_b},
  };
  for (const auto& cd : cls_diffs) {
    H5P_genclass_t* other =
        H5P__create_class(nullptr, "cmp_meta", H5P_TYPE_USER, cd.cf, cd.cd,
                          nullptr, nullptr, cd.clf, cd.cld);
    ASSERT_NE(other, nullptr);
    absl::Cleanup close_other = [other] { H5P__close_class(other); };
    EXPECT_NE(H5P__cmp_class(c1, other), 0);
    EXPECT_NE(H5P__cmp_class(other, c1), 0);
  }

  // Test H5P__cmp_plist differences: nprops (<, >), missing prop, differing
  // value, and differing class.
  ScopedPclass pcls1(H5Pcreate_class(H5P_ROOT, "plcmp1", nullptr, nullptr,
                                     nullptr, nullptr, nullptr, nullptr));
  ScopedPclass pcls2(H5Pcreate_class(H5P_ROOT, "plcmp2", nullptr, nullptr,
                                     nullptr, nullptr, nullptr, nullptr));
  ASSERT_TRUE(pcls1.valid() && pcls2.valid());
  ASSERT_GE(H5Pregister2(pcls1.get(), "k", sizeof(v1), &v1, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
            0);
  ASSERT_GE(H5Pregister2(pcls2.get(), "k", sizeof(v1), &v1, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
            0);

  ScopedPlist l1(H5Pcreate(pcls1.get()));
  ScopedPlist l1_b(H5Pcreate(pcls1.get()));
  ScopedPlist l2(H5Pcreate(pcls2.get()));
  ASSERT_TRUE(l1.valid() && l1_b.valid() && l2.valid());

  EXPECT_GT(H5Pequal(l1.get(), l1_b.get()), 0);
  // Same properties, different class -> unequal.
  EXPECT_EQ(H5Pequal(l1.get(), l2.get()), 0);

  // Differing value.
  ASSERT_GE(H5Pset(l1_b.get(), "k", &v2), 0);
  EXPECT_EQ(H5Pequal(l1.get(), l1_b.get()), 0);

  // Same nprops, different property name in l1 vs l1_b.
  ASSERT_GE(H5Premove(l1_b.get(), "k"), 0);
  ASSERT_GE(H5Pinsert2(l1_b.get(), "other_k", sizeof(v1), &v1, nullptr, nullptr,
                       nullptr, nullptr, nullptr, nullptr),
            0);
  EXPECT_EQ(H5Pequal(l1.get(), l1_b.get()), 0);

  // Differing nprops.
  ASSERT_GE(H5Pinsert2(l1_b.get(), "k", sizeof(v1), &v1, nullptr, nullptr,
                       nullptr, nullptr, nullptr, nullptr),
            0);
  EXPECT_EQ(H5Pequal(l1.get(), l1_b.get()), 0);
  EXPECT_EQ(H5Pequal(l1_b.get(), l1.get()), 0);
}

TEST(H5PintTest, IteratePlistAndPclassWithStopResumeAndNonDefaultOnly) {
  ASSERT_GE(H5open(), 0);

  ScopedPclass par(H5Pcreate_class(H5P_ROOT, "iter_par", nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr));
  ASSERT_TRUE(par.valid());
  int v = 1;
  ASSERT_GE(H5Pregister2(par.get(), "a_par", sizeof(v), &v, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
            0);
  ASSERT_GE(H5Pregister2(par.get(), "b_shadow", sizeof(v), &v, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
            0);

  ScopedPclass cls(H5Pcreate_class(par.get(), "iter_cls", nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr));
  ASSERT_TRUE(cls.valid());
  ASSERT_GE(H5Pregister2(cls.get(), "b_shadow", sizeof(v), &v, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
            0);
  ASSERT_GE(H5Pregister2(cls.get(), "c_cls", sizeof(v), &v, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
            0);
  ASSERT_GE(H5Pregister2(cls.get(), "d_del", sizeof(v), &v, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
            0);

  ScopedPlist pl(H5Pcreate(cls.get()));
  ASSERT_TRUE(pl.valid());
  int v2 = 2;
  ASSERT_GE(H5Pset(pl.get(), "b_shadow", &v2), 0);
  ASSERT_GE(H5Premove(pl.get(), "d_del"), 0);

  struct IterState {
    std::vector<std::string> names;
    int stop_after = -1;
    int return_code = 1;
  } st;

  auto pub_cb = [](hid_t, const char* name, void* data) -> herr_t {
    auto* s = static_cast<IterState*>(data);
    s->names.emplace_back(name);
    if (s->stop_after >= 0 &&
        static_cast<int>(s->names.size()) >= s->stop_after) {
      return s->return_code;
    }
    return 0;
  };

  // Iterate all properties of plist with idx == nullptr (should skip d_del and
  // deduplicate b_shadow).
  EXPECT_EQ(H5Piterate(pl.get(), nullptr, pub_cb, &st), 0);
  EXPECT_THAT(st.names, ElementsAre("b_shadow", "c_cls", "a_par"));

  // Iterate plist with pause and resume via idx.
  st.names.clear();
  st.stop_after = 2;
  int idx = 0;
  EXPECT_EQ(H5Piterate(pl.get(), &idx, pub_cb, &st), 1);
  EXPECT_EQ(idx, 1);
  EXPECT_THAT(st.names, ElementsAre("b_shadow", "c_cls"));

  // Resume from idx + 1 to finish remaining property.
  ++idx;
  st.stop_after = -1;
  EXPECT_EQ(H5Piterate(pl.get(), &idx, pub_cb, &st), 0);
  EXPECT_THAT(st.names, ElementsAre("b_shadow", "c_cls", "a_par"));

  // Internal H5P__iterate_plist with iter_all_prop == false visits ONLY changed
  // properties in plist->props.
  auto* plist = static_cast<H5P_genplist_t*>(
      H5I_object_verify(pl.get(), H5I_GENPROP_LST));
  ASSERT_NE(plist, nullptr);
  std::vector<std::string> changed_only;
  int int_idx = 0;
  auto int_cb = [](H5P_genprop_t* prop, void* data) -> int {
    static_cast<std::vector<std::string>*>(data)->emplace_back(prop->name);
    return 0;
  };
  EXPECT_EQ(H5P__iterate_plist(plist, /*iter_all_prop=*/false, &int_idx, int_cb,
                               &changed_only),
            0);
  EXPECT_THAT(changed_only, ElementsAre("b_shadow"));

  // Iterate over property class (with nullptr idx, stop/resume, and negative).
  // Skip-list traversal follows string-hash order, so capture full order first.
  st.names.clear();
  EXPECT_EQ(H5Piterate(cls.get(), nullptr, pub_cb, &st), 0);
  const std::vector<std::string> full_cls_order = st.names;
  EXPECT_THAT(full_cls_order,
              UnorderedElementsAre("b_shadow", "c_cls", "d_del"));

  st.names.clear();
  st.stop_after = 1;
  idx = 0;
  EXPECT_EQ(H5Piterate(cls.get(), &idx, pub_cb, &st), 1);
  ++idx;
  st.stop_after = -1;
  EXPECT_EQ(H5Piterate(cls.get(), &idx, pub_cb, &st), 0);
  EXPECT_EQ(st.names, full_cls_order);

  {
    ScopedErrorSilence silence;
    st.names.clear();
    st.stop_after = 1;
    st.return_code = -1;
    idx = 0;
    EXPECT_LT(H5Piterate(cls.get(), &idx, pub_cb, &st), 0);
    idx = 0;
    EXPECT_LT(H5Piterate(pl.get(), &idx, pub_cb, &st), 0);
  }
}

TEST(H5PintTest, InternalHelpersNewPlistOfTypeDefaultVerifyAndPackageTerm) {
  ASSERT_GE(H5open(), 0);

  // Verify H5P__new_plist_of_type across all 21 valid library property types.
  for (int t = H5P_TYPE_OBJECT_CREATE; t <= H5P_TYPE_REFERENCE_ACCESS; ++t) {
    ScopedPlist pl(H5P__new_plist_of_type(static_cast<H5P_plist_type_t>(t)));
    EXPECT_TRUE(pl.valid()) << "Failed for type " << t;
  }

  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5P__new_plist_of_type(H5P_TYPE_USER), H5I_INVALID_HID);
    EXPECT_EQ(H5P__new_plist_of_type(H5P_TYPE_ROOT), H5I_INVALID_HID);
    EXPECT_EQ(H5P__new_plist_of_type(H5P_TYPE_MAX_TYPE), H5I_INVALID_HID);
  }

  // Default plist detection, H5P_object_verify, H5P_get_plist_id,
  // H5P_ignore_cmp.
  EXPECT_TRUE(H5P_is_default_plist(H5P_DEFAULT));
  EXPECT_TRUE(H5P_is_default_plist(H5P_FILE_CREATE_DEFAULT));
  EXPECT_TRUE(H5P_is_default_plist(H5P_DATASET_ACCESS_DEFAULT));

  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  EXPECT_FALSE(H5P_is_default_plist(fcpl.get()));

  H5P_genplist_t* verified =
      H5P_object_verify(fcpl.get(), H5P_FILE_CREATE, /*allow_default=*/false);
  ASSERT_NE(verified, nullptr);
  EXPECT_EQ(H5P_get_plist_id(verified), fcpl.get());
  EXPECT_NE(H5P_get_class(verified), nullptr);
  EXPECT_NE(H5P_object_verify(H5P_FILE_CREATE_DEFAULT, H5P_FILE_CREATE,
                              /*allow_default=*/true),
            nullptr);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5P_object_verify(H5P_FILE_CREATE_DEFAULT, H5P_FILE_CREATE,
                                /*allow_default=*/false),
              nullptr);
    EXPECT_EQ(H5P_object_verify(fcpl.get(), H5P_FILE_ACCESS,
                                /*allow_default=*/true),
              nullptr);
  }
  EXPECT_EQ(H5P_ignore_cmp(nullptr, nullptr, 0), 0);

  // Close open handles, exercise H5close() / H5P_term_package() / H5open(),
  // and verify H5open() cleanly restores library state for subsequent tests.
  fcpl.reset();
  absl::Cleanup reopen = [] { H5open(); };
  ASSERT_GE(H5close(), 0);
  EXPECT_EQ(H5P_term_package(), 0);
  ASSERT_GE(H5open(), 0);
}

}  // namespace
}  // namespace hdf5
