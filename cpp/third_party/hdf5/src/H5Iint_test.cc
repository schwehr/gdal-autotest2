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
// Tests internal and package H5I routines: type/slot management, mark-and-sweep
// clearing, re-entrant removal, future IDs, refcounting, iteration, unwrapping,
// pointer lookup, and package termination (third_party/hdf5/src/H5Iint.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Iint.c

#include <array>
#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5I_INT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5I_FRIEND
#include "third_party/hdf5/src/H5Ipkg.h"
#include "third_party/hdf5/src/H5Tprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5I_INT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5I_INT_TEST_RESTORE_UNDEF_NDEBUG
#endif

extern "C" int H5I_term_package(void);

namespace hdf5 {
namespace {

#if defined(__clang__)
#define H5I_TEST_NO_SANITIZE_ENUM __attribute__((no_sanitize("enum")))
#else
#define H5I_TEST_NO_SANITIZE_ENUM
#endif

// H5I_type_t is defined without a fixed underlying type (enumerators [-2, 17]),
// so C++ -fsanitize=enum traps when values >= 32 (up to H5I_MAX_NUM_TYPES) or
// invalid test values (e.g. 999) touch H5I_type_t directly. Keep type values in
// int outside these annotated C-boundary helpers.
H5I_TEST_NO_SANITIZE_ENUM int RegisterTypeCommonRaw(unsigned reserved,
                                                     H5I_free_t free_func) {
  return static_cast<int>(H5I__register_type_common(reserved, free_func));
}

H5I_TEST_NO_SANITIZE_ENUM void InitClassTypeRaw(H5I_class_t* cls, int type) {
  cls->type = static_cast<H5I_type_t>(type);
}

H5I_TEST_NO_SANITIZE_ENUM herr_t DestroyTypeRaw(int type) {
  return H5I__destroy_type(static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM int64_t NmembersRaw(int type) {
  return H5I_nmembers(static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM herr_t ClearTypeRaw(int type, bool force,
                                              bool app_ref) {
  return H5I_clear_type(static_cast<H5I_type_t>(type), force, app_ref);
}

H5I_TEST_NO_SANITIZE_ENUM hid_t RegisterInternalRaw(
    int type, const void* obj, bool app_ref,
    H5I_future_realize_func_t realize_cb,
    H5I_future_discard_func_t discard_cb) {
  return H5I__register(static_cast<H5I_type_t>(type), obj, app_ref, realize_cb,
                       discard_cb);
}

H5I_TEST_NO_SANITIZE_ENUM hid_t RegisterLibRaw(int type, const void* obj,
                                               bool app_ref) {
  return H5I_register(static_cast<H5I_type_t>(type), obj, app_ref);
}

H5I_TEST_NO_SANITIZE_ENUM herr_t RegisterExistingRaw(int type, void* obj,
                                                     bool app_ref,
                                                     hid_t existing_id) {
  return H5I_register_using_existing_id(static_cast<H5I_type_t>(type), obj,
                                        app_ref, existing_id);
}

H5I_TEST_NO_SANITIZE_ENUM void* ObjectVerifyRaw(hid_t id, int type) {
  return H5I_object_verify(id, static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM void* RemoveVerifyRaw(hid_t id, int type) {
  return H5I__remove_verify(id, static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM int IncTypeRefRaw(int type) {
  return H5I__inc_type_ref(static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM int DecTypeRefRaw(int type) {
  return H5I_dec_type_ref(static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM int GetTypeRefRaw(int type) {
  return H5I__get_type_ref(static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM herr_t IterateRaw(int type, H5I_search_func_t func,
                                            void* udata, bool app_ref) {
  return H5I_iterate(static_cast<H5I_type_t>(type), func, udata, app_ref);
}

H5I_TEST_NO_SANITIZE_ENUM herr_t FindIdRaw(const void* obj, int type,
                                           hid_t* id) {
  return H5I_find_id(obj, static_cast<H5I_type_t>(type), id);
}

struct FreeTracker {
  std::vector<int> tags;
  bool should_fail = false;
  void** last_request = nullptr;
};

struct TrackedObj {
  FreeTracker* tracker = nullptr;
  int tag = 0;
};

herr_t FreeTrackedCallback(void* obj, void** request) {
  auto* item = static_cast<TrackedObj*>(obj);
  item->tracker->last_request = request;
  if (item->tracker->should_fail) {
    return -1;
  }
  item->tracker->tags.push_back(item->tag);
  return 0;
}

struct ReentrantCtx {
  hid_t peer_id = H5I_INVALID_HID;
  int freed_self = 0;
  int removed_peer = 0;
};

herr_t ReentrantRemoveFreeCallback(void* obj, void** /*request*/) {
  auto* ctx = static_cast<ReentrantCtx*>(obj);
  ++ctx->freed_self;
  if (ctx->peer_id != H5I_INVALID_HID &&
      H5I_remove(ctx->peer_id) != nullptr) {
    ++ctx->removed_peer;
    ctx->peer_id = H5I_INVALID_HID;
  }
  return 0;
}

struct FutureCase {
  int target_type = static_cast<int>(H5I_BADID);
  TrackedObj* actual_obj = nullptr;
  int realize_calls = 0;
  int discard_calls = 0;
  bool fail_realize = false;
  bool return_invalid_id = false;
  bool fail_discard = false;
};

herr_t RealizeCaseCb(void* obj, hid_t* actual_id) {
  auto* fc = static_cast<FutureCase*>(obj);
  ++fc->realize_calls;
  if (fc->fail_realize) {
    return -1;
  }
  if (fc->return_invalid_id) {
    *actual_id = H5I_INVALID_HID;
    return 0;
  }
  *actual_id =
      RegisterInternalRaw(fc->target_type, fc->actual_obj, true, nullptr,
                          nullptr);
  return (*actual_id >= 0) ? 0 : -1;
}

herr_t DiscardCaseCb(void* obj) {
  auto* fc = static_cast<FutureCase*>(obj);
  ++fc->discard_calls;
  return fc->fail_discard ? -1 : 0;
}

TEST(H5IintTest, RegisterTypeCommonReuseAndStaticClassRefCount) {
  // Fill all application type slots until H5I__register_type_common fails.
  std::vector<int> slots;
  absl::Cleanup cleanup_slots = [&slots] {
    for (const int t : slots) {
      if (t > 0 && H5I_type_info_array_g[t] != nullptr) {
        DestroyTypeRaw(t);
      }
    }
  };

  while (true) {
    const int t = RegisterTypeCommonRaw(0, nullptr);
    if (t == static_cast<int>(H5I_BADID)) {
      break;
    }
    slots.push_back(t);
    ASSERT_LE(slots.size(), 256);
  }
  ASSERT_GE(slots.size(), 2);

  // Destroy one slot while H5I_next_type_g == H5I_MAX_NUM_TYPES and re-register
  // to cover the free-slot search branch in H5I__register_type_common.
  const int freed_slot = slots.back();
  slots.pop_back();
  ASSERT_GE(DestroyTypeRaw(freed_slot), 0);

  const int reused = RegisterTypeCommonRaw(1, nullptr);
  EXPECT_EQ(reused, freed_slot);
  slots.push_back(reused);

  // Exhaustion fails cleanly with H5E_NOSPACE.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(RegisterTypeCommonRaw(0, nullptr), static_cast<int>(H5I_BADID));
  }

  // Test non-APPLICATION class via H5I_register_type on the same freed slot.
  slots.pop_back();
  ASSERT_GE(DestroyTypeRaw(reused), 0);

  H5I_class_t static_cls{};
  InitClassTypeRaw(&static_cls, reused);
  static_cls.flags = 0;  // Not H5I_CLASS_IS_APPLICATION.
  static_cls.reserved = 2;
  ASSERT_GE(H5I_register_type(&static_cls), 0);
  EXPECT_EQ(GetTypeRefRaw(reused), 1);

  // Re-registering same class increments init_count without re-initializing.
  ASSERT_GE(H5I_register_type(&static_cls), 0);
  EXPECT_EQ(GetTypeRefRaw(reused), 2);
  EXPECT_EQ(DecTypeRefRaw(reused), 1);
  EXPECT_EQ(DecTypeRefRaw(reused), 0);
  EXPECT_EQ(H5I_type_info_array_g[reused], nullptr);
}

TEST(H5IintTest, NmembersClearMarkAndSweepAndReentrantRemove) {
  FreeTracker tracker;
  TrackedObj a{&tracker, 1};
  TrackedObj b{&tracker, 2};
  TrackedObj c{&tracker, 3};

  const int type = RegisterTypeCommonRaw(0, FreeTrackedCallback);
  ASSERT_GE(type, static_cast<int>(H5I_NTYPES));
  absl::Cleanup destroy = [type, &tracker] {
    tracker.should_fail = false;
    if (H5I_type_info_array_g[type] != nullptr) {
      DestroyTypeRaw(type);
    }
  };

  EXPECT_EQ(NmembersRaw(type), 0);
  EXPECT_EQ(H5I_nmembers(H5I_MAP), 0);

  const hid_t id_app_int =
      RegisterInternalRaw(type, &a, /*app_ref=*/true, nullptr, nullptr);
  ASSERT_GE(id_app_int, 0);
  // count == 2, app_count == 1 -> survives app_ref == true, cleared when
  // app_ref == false, force == false because (count - (!app_ref * app_count))
  // == 1.
  EXPECT_EQ(H5I_inc_ref(id_app_int, /*app_ref=*/false), 2);

  const hid_t id_two_int =
      RegisterInternalRaw(type, &b, /*app_ref=*/false, nullptr, nullptr);
  ASSERT_GE(id_two_int, 0);
  // count == 2, app_count == 0 -> survives both app_ref == true and
  // app_ref == false unless force == true.
  EXPECT_EQ(H5I_inc_ref(id_two_int, /*app_ref=*/false), 2);

  // Failing free_func prevents clearing unless force == true.
  const hid_t id_fail_free =
      RegisterInternalRaw(type, &c, /*app_ref=*/true, nullptr, nullptr);
  ASSERT_GE(id_fail_free, 0);
  EXPECT_EQ(NmembersRaw(type), 3);

  tracker.should_fail = true;
  EXPECT_GE(ClearTypeRaw(type, /*force=*/false, /*app_ref=*/false), 0);
  EXPECT_EQ(NmembersRaw(type), 3);
  tracker.should_fail = false;

  // With app_ref == true and force == false, id_fail_free (count == 1) is
  // cleared while id_app_int (count == 2) and id_two_int (count == 2) survive.
  EXPECT_GE(ClearTypeRaw(type, /*force=*/false, /*app_ref=*/true), 0);
  EXPECT_EQ(NmembersRaw(type), 2);

  // With app_ref == false and force == false, id_app_int is cleared
  // (count - app_count == 1) while id_two_int (count - 0 == 2) survives.
  EXPECT_GE(ClearTypeRaw(type, /*force=*/false, /*app_ref=*/false), 0);
  EXPECT_EQ(NmembersRaw(type), 1);
  EXPECT_EQ(H5I_object(id_two_int), &b);

  // force == true clears even when free_func fails.
  tracker.should_fail = true;
  EXPECT_GE(ClearTypeRaw(type, /*force=*/true, /*app_ref=*/true), 0);
  EXPECT_EQ(NmembersRaw(type), 0);
  tracker.should_fail = false;

  // Re-entrant H5I_remove inside free_func during H5I_clear_type exercises the
  // H5I_marking_s mark-instead-of-delete branch in H5I__remove_common.
  const int reent_type = RegisterTypeCommonRaw(0, ReentrantRemoveFreeCallback);
  ASSERT_GE(reent_type, static_cast<int>(H5I_NTYPES));
  absl::Cleanup destroy_reent = [reent_type] {
    if (H5I_type_info_array_g[reent_type] != nullptr) {
      DestroyTypeRaw(reent_type);
    }
  };

  ReentrantCtx r1{};
  ReentrantCtx r2{};
  const hid_t rid1 =
      RegisterInternalRaw(reent_type, &r1, true, nullptr, nullptr);
  const hid_t rid2 =
      RegisterInternalRaw(reent_type, &r2, true, nullptr, nullptr);
  ASSERT_GE(rid1, 0);
  ASSERT_GE(rid2, 0);
  r1.peer_id = rid2;
  r2.peer_id = rid1;

  ASSERT_GE(ClearTypeRaw(reent_type, /*force=*/true, /*app_ref=*/false), 0);
  EXPECT_EQ(NmembersRaw(reent_type), 0);
  EXPECT_EQ(r1.freed_self + r2.freed_self, 1);
  EXPECT_EQ(r1.removed_peer + r2.removed_peer, 1);

  // Destroyed slot hits type_info == NULL branches cleanly.
  const int gone_type = RegisterTypeCommonRaw(0, nullptr);
  ASSERT_GE(gone_type, static_cast<int>(H5I_NTYPES));
  ASSERT_GE(DestroyTypeRaw(gone_type), 0);
  EXPECT_EQ(NmembersRaw(gone_type), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5I_nmembers(H5I_BADID), 0);
  EXPECT_LT(NmembersRaw(999), 0);
  EXPECT_LT(H5I_clear_type(H5I_BADID, false, false), 0);
  EXPECT_LT(ClearTypeRaw(999, false, false), 0);
  EXPECT_LT(ClearTypeRaw(gone_type, false, false), 0);
  EXPECT_LT(H5I__destroy_type(H5I_BADID), 0);
  EXPECT_LT(DestroyTypeRaw(999), 0);
  EXPECT_LT(DestroyTypeRaw(gone_type), 0);
}

TEST(H5IintTest, RegisterExistingSubstRemoveVerifyAndErrorPaths) {
  ASSERT_GE(H5open(), 0);
  const int type1 = RegisterTypeCommonRaw(0, nullptr);
  const int type2 = RegisterTypeCommonRaw(0, nullptr);
  ASSERT_GE(type1, static_cast<int>(H5I_NTYPES));
  ASSERT_GE(type2, static_cast<int>(H5I_NTYPES));
  absl::Cleanup cleanup = [type1, type2] {
    if (H5I_type_info_array_g[type1] != nullptr) DestroyTypeRaw(type1);
    if (H5I_type_info_array_g[type2] != nullptr) DestroyTypeRaw(type2);
  };

  int obj_a = 1;
  int obj_b = 2;
  const hid_t id1 = RegisterInternalRaw(type1, &obj_a, true, nullptr, nullptr);
  const hid_t id2 =
      RegisterInternalRaw(type1, &obj_b, false, nullptr, nullptr);
  ASSERT_GE(id1, 0);
  ASSERT_GE(id2, 0);

  // Cache hit branch on H5I__find_id + subst.
  EXPECT_EQ(H5I_object(id2), &obj_b);
  EXPECT_EQ(H5I_object(id2), &obj_b);
  EXPECT_EQ(H5I_subst(id1, &obj_b), &obj_a);
  EXPECT_EQ(ObjectVerifyRaw(id1, type1), &obj_b);
  EXPECT_EQ(ObjectVerifyRaw(id1, type2), nullptr);

  // Remove id1 while last_id_info == id1, then re-register using existing_id.
  EXPECT_EQ(RemoveVerifyRaw(id1, type2), nullptr);
  EXPECT_EQ(RemoveVerifyRaw(id1, type1), &obj_b);
  ASSERT_GE(RegisterExistingRaw(type1, &obj_a, true, id1), 0);
  EXPECT_EQ(H5I_object(id1), &obj_a);

  // Exercise H5I_register + H5I_is_file_object(H5I_MAP) while preserving
  // global H5I_MAP registration state, and temporarily uninitialize H5I_MAP to
  // cover the H5I_register() failure branch cleanly.
  const H5I_class_t* saved_map_cls =
      H5I_type_info_array_g[H5I_MAP] != nullptr
          ? H5I_type_info_array_g[H5I_MAP]->cls
          : nullptr;
  const unsigned saved_map_init_count =
      H5I_type_info_array_g[H5I_MAP] != nullptr
          ? H5I_type_info_array_g[H5I_MAP]->init_count
          : 0;
  H5I_class_t fallback_map_cls{};
  fallback_map_cls.type = H5I_MAP;
  absl::Cleanup restore_map = [saved_map_cls, saved_map_init_count] {
    while (H5I_type_info_array_g[H5I_MAP] != nullptr &&
           H5I_type_info_array_g[H5I_MAP]->init_count > 0) {
      H5I_dec_type_ref(H5I_MAP);
    }
    if (saved_map_cls != nullptr) {
      for (unsigned i = 0; i < saved_map_init_count; ++i) {
        H5I_register_type(saved_map_cls);
      }
    }
  };
  if (saved_map_cls == nullptr) {
    ASSERT_GE(H5I_register_type(&fallback_map_cls), 0);
  }
  const hid_t map_id = H5I_register(H5I_MAP, &obj_a, /*app_ref=*/false);
  ASSERT_GE(map_id, 0);
  EXPECT_GT(H5I_is_file_object(map_id), 0);
  EXPECT_EQ(H5I_remove(map_id), &obj_a);

  while (H5I_type_info_array_g[H5I_MAP] != nullptr &&
         H5I_type_info_array_g[H5I_MAP]->init_count > 0) {
    ASSERT_GE(H5I_dec_type_ref(H5I_MAP), 0);
  }

  // Create a destroyed user type slot to hit type_info == NULL branches.
  const int gone_type = RegisterTypeCommonRaw(0, nullptr);
  ASSERT_GE(gone_type, static_cast<int>(H5I_NTYPES));
  const hid_t gone_id =
      RegisterInternalRaw(gone_type, &obj_a, true, nullptr, nullptr);
  ASSERT_GE(gone_id, 0);
  ASSERT_GE(DestroyTypeRaw(gone_type), 0);

  ScopedErrorSilence silence;
  EXPECT_EQ(RegisterLibRaw(static_cast<int>(H5I_MAP), &obj_a, false),
            H5I_INVALID_HID);
  EXPECT_EQ(H5I__register(H5I_BADID, &obj_a, true, nullptr, nullptr),
            H5I_INVALID_HID);
  EXPECT_EQ(RegisterInternalRaw(gone_type, &obj_a, true, nullptr, nullptr),
            H5I_INVALID_HID);

  EXPECT_LT(RegisterExistingRaw(type1, &obj_a, true, id1), 0);
  EXPECT_LT(H5I_register_using_existing_id(H5I_BADID, &obj_a, true, gone_id),
            0);
  EXPECT_LT(RegisterExistingRaw(gone_type, &obj_a, true, gone_id), 0);
  EXPECT_LT(RegisterExistingRaw(type2, &obj_a, true, gone_id), 0);

  EXPECT_EQ(H5I_subst(H5I_INVALID_HID, &obj_a), nullptr);
  EXPECT_EQ(H5I_object(H5I_INVALID_HID), nullptr);
  EXPECT_EQ(H5I_object(gone_id), nullptr);
  EXPECT_EQ(H5I_get_type(H5I_INVALID_HID), H5I_BADID);

  EXPECT_EQ(H5I_remove(H5I_INVALID_HID), nullptr);
  EXPECT_EQ(H5I_remove(static_cast<hid_t>(1) << 56), nullptr);
  EXPECT_EQ(H5I_remove(gone_id), nullptr);
  EXPECT_EQ(H5I_remove(id1), &obj_a);
  EXPECT_EQ(H5I_remove(id1), nullptr);
}

TEST(H5IintTest, FutureIdMarkClearAndFindFailurePaths) {
  FreeTracker tracker;
  TrackedObj actual{&tracker, 42};

  const int type1 = RegisterTypeCommonRaw(0, FreeTrackedCallback);
  const int type2 = RegisterTypeCommonRaw(0, nullptr);
  ASSERT_GE(type1, static_cast<int>(H5I_NTYPES));
  ASSERT_GE(type2, static_cast<int>(H5I_NTYPES));
  absl::Cleanup cleanup = [type1, type2] {
    if (H5I_type_info_array_g[type1] != nullptr) DestroyTypeRaw(type1);
    if (H5I_type_info_array_g[type2] != nullptr) DestroyTypeRaw(type2);
  };

  ScopedErrorSilence silence;

  // 1. realize_cb returns < 0.
  FutureCase fail_realize{type1, &actual, 0, 0, true, false, false};
  const hid_t fid_fail_r = RegisterInternalRaw(
      type1, &fail_realize, true, RealizeCaseCb, DiscardCaseCb);
  ASSERT_GE(fid_fail_r, 0);
  EXPECT_EQ(H5I__find_id(fid_fail_r), nullptr);
  EXPECT_EQ(fail_realize.realize_calls, 1);

  // 2. realize_cb returns H5I_INVALID_HID.
  FutureCase invalid_id{type1, &actual, 0, 0, false, true, false};
  const hid_t fid_inv =
      RegisterInternalRaw(type1, &invalid_id, true, RealizeCaseCb,
                          DiscardCaseCb);
  ASSERT_GE(fid_inv, 0);
  EXPECT_EQ(H5I__find_id(fid_inv), nullptr);

  // 3. realize_cb returns ID of wrong type.
  FutureCase mismatch{type2, &actual, 0, 0, false, false, false};
  const hid_t fid_mis =
      RegisterInternalRaw(type1, &mismatch, true, RealizeCaseCb, DiscardCaseCb);
  ASSERT_GE(fid_mis, 0);
  EXPECT_EQ(H5I__find_id(fid_mis), nullptr);

  // 4. H5I_clear_type on unrealized future IDs: those with succeeding
  // discard_cb are removed when force == false, leaving only fid_df until
  // force == true.
  FutureCase discard_fail{type1, &actual, 0, 0, false, false, true};
  const hid_t fid_df = RegisterInternalRaw(type1, &discard_fail, true,
                                           RealizeCaseCb, DiscardCaseCb);
  ASSERT_GE(fid_df, 0);
  EXPECT_GE(ClearTypeRaw(type1, /*force=*/false, /*app_ref=*/false), 0);
  EXPECT_EQ(NmembersRaw(type1), 1);
  EXPECT_GE(ClearTypeRaw(type1, /*force=*/true, /*app_ref=*/false), 0);
  EXPECT_EQ(NmembersRaw(type1), 0);

  // 5. Realize succeeds, discard_cb succeeds -> converts from future to actual.
  FutureCase ok_case{type1, &actual, 0, 0, false, false, false};
  const hid_t fid_ok =
      RegisterInternalRaw(type1, &ok_case, true, RealizeCaseCb, DiscardCaseCb);
  ASSERT_GE(fid_ok, 0);
  H5I_id_info_t* info = H5I__find_id(fid_ok);
  ASSERT_NE(info, nullptr);
  EXPECT_FALSE(info->is_future);
  EXPECT_EQ(ok_case.realize_calls, 1);
  EXPECT_EQ(ok_case.discard_calls, 1);
  EXPECT_EQ(H5I_object(fid_ok), &actual);
  EXPECT_EQ(ok_case.realize_calls, 1);

  // 6. Realize succeeds, discard_cb fails during H5I__find_id.
  FutureCase fail_disc_find{type1, &actual, 0, 0, false, false, true};
  const hid_t fid_fdf = RegisterInternalRaw(type1, &fail_disc_find, true,
                                            RealizeCaseCb, DiscardCaseCb);
  ASSERT_GE(fid_fdf, 0);
  EXPECT_EQ(H5I__find_id(fid_fdf), nullptr);
  EXPECT_EQ(fail_disc_find.discard_calls, 1);
  EXPECT_EQ(H5I_remove(fid_fdf), &actual);

  // 7. H5I_clear_type discards an unrealized future ID cleanly.
  FutureCase clear_ok{type1, &actual, 0, 0, false, false, false};
  const hid_t fid_cok = RegisterInternalRaw(type1, &clear_ok, true,
                                            RealizeCaseCb, DiscardCaseCb);
  ASSERT_GE(fid_cok, 0);
  EXPECT_GE(ClearTypeRaw(type1, false, false), 0);
  EXPECT_EQ(clear_ok.discard_calls, 1);
}

TEST(H5IintTest, InternalRefCountingAsyncAndAlwaysClose) {
  FreeTracker tracker;
  TrackedObj a{&tracker, 11};
  TrackedObj b{&tracker, 22};

  const int type_cb = RegisterTypeCommonRaw(0, FreeTrackedCallback);
  const int type_nocb = RegisterTypeCommonRaw(0, nullptr);
  ASSERT_GE(type_cb, static_cast<int>(H5I_NTYPES));
  ASSERT_GE(type_nocb, static_cast<int>(H5I_NTYPES));
  absl::Cleanup cleanup = [type_cb, type_nocb, &tracker] {
    tracker.should_fail = false;
    if (H5I_type_info_array_g[type_cb] != nullptr) DestroyTypeRaw(type_cb);
    if (H5I_type_info_array_g[type_nocb] != nullptr) {
      DestroyTypeRaw(type_nocb);
    }
  };

  // Non-app vs app refs + async token passthrough.
  const hid_t id =
      RegisterInternalRaw(type_cb, &a, /*app_ref=*/true, nullptr, nullptr);
  ASSERT_GE(id, 0);
  EXPECT_EQ(H5I_inc_ref(id, /*app_ref=*/false), 2);
  EXPECT_EQ(H5I_inc_ref(id, /*app_ref=*/true), 2);
  EXPECT_EQ(H5I_get_ref(id, /*app_ref=*/false), 3);
  EXPECT_EQ(H5I_get_ref(id, /*app_ref=*/true), 2);

  EXPECT_EQ(H5I_dec_ref(id), 2);
  void* async_token = nullptr;
  EXPECT_EQ(H5I_dec_app_ref_async(id, &async_token), 1);
  EXPECT_EQ(H5I_dec_app_ref_async(id, &async_token), 0);
  EXPECT_EQ(tracker.last_request, &async_token);
  ASSERT_EQ(tracker.tags.size(), 1);
  EXPECT_EQ(tracker.tags[0], 11);

  // free_func == nullptr closes without callback.
  const hid_t id_nocb =
      RegisterInternalRaw(type_nocb, &b, true, nullptr, nullptr);
  ASSERT_GE(id_nocb, 0);
  EXPECT_EQ(H5I_dec_app_ref(id_nocb), 0);

  // always_close removes IDs even when free_func returns < 0.
  const hid_t id_ac_sync =
      RegisterInternalRaw(type_cb, &a, true, nullptr, nullptr);
  const hid_t id_ac_async =
      RegisterInternalRaw(type_cb, &b, true, nullptr, nullptr);
  ASSERT_GE(id_ac_sync, 0);
  ASSERT_GE(id_ac_async, 0);

  EXPECT_EQ(H5I_dec_app_ref_always_close(id_ac_sync), 0);
  tracker.should_fail = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5I_dec_ref(id_ac_async), 0);
    EXPECT_LT(H5I_dec_app_ref_async(id_ac_async, &async_token), 0);
    EXPECT_LT(H5I_dec_app_ref(id_ac_async), 0);
    EXPECT_LT(H5I_dec_app_ref_always_close_async(id_ac_async, &async_token), 0);
  }
  tracker.should_fail = false;
  EXPECT_EQ(H5I_object(id_ac_async), nullptr);

  const hid_t id_ac_sync_fail =
      RegisterInternalRaw(type_cb, &a, true, nullptr, nullptr);
  ASSERT_GE(id_ac_sync_fail, 0);
  tracker.should_fail = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5I_dec_app_ref_always_close(id_ac_sync_fail), 0);
  }
  tracker.should_fail = false;
  EXPECT_EQ(H5I_object(id_ac_sync_fail), nullptr);

  // Type ref error checks.
  const int dead_type = RegisterTypeCommonRaw(0, nullptr);
  ASSERT_GE(dead_type, static_cast<int>(H5I_NTYPES));
  EXPECT_EQ(IncTypeRefRaw(dead_type), 2);
  EXPECT_EQ(DecTypeRefRaw(dead_type), 1);
  EXPECT_EQ(DecTypeRefRaw(dead_type), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5I_dec_ref(id), 0);
  EXPECT_LT(H5I_dec_app_ref(id), 0);
  EXPECT_LT(H5I_dec_app_ref_async(id, &async_token), 0);
  EXPECT_LT(H5I_dec_app_ref_always_close(id), 0);
  EXPECT_LT(H5I_dec_app_ref_always_close_async(id, &async_token), 0);
  EXPECT_LT(H5I_inc_ref(id, true), 0);
  EXPECT_LT(H5I_get_ref(id, true), 0);

  EXPECT_LT(IncTypeRefRaw(dead_type), 0);
  EXPECT_LT(GetTypeRefRaw(dead_type), 0);
  EXPECT_LT(DecTypeRefRaw(dead_type), 0);
  EXPECT_LT(H5I_dec_type_ref(H5I_BADID), 0);
  EXPECT_LT(DecTypeRefRaw(999), 0);
}

TEST(H5IintTest, IterateUnwrapFindIdAndIsFileObject) {
  ScopedFile file = CreateInMemoryFile("h5iint_unwrap.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup group(H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
  ASSERT_TRUE(group.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "/d", H5T_NATIVE_INT, space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedType committed_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(committed_dt.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/dt", committed_dt.get(), H5P_DEFAULT,
                      H5P_DEFAULT, H5P_DEFAULT),
            0);
  ScopedType transient_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(transient_dt.valid());

  // H5I_is_file_object coverage across library types.
  EXPECT_GT(H5I_is_file_object(group.get()), 0);
  EXPECT_GT(H5I_is_file_object(dset.get()), 0);
  EXPECT_GT(H5I_is_file_object(committed_dt.get()), 0);
  EXPECT_EQ(H5I_is_file_object(transient_dt.get()), 0);
  EXPECT_EQ(H5I_is_file_object(file.get()), 0);
  EXPECT_EQ(H5I_is_file_object(space.get()), 0);

  // Closed datatype ID still encodes H5I_DATATYPE so H5I_object returns nullptr.
  ScopedType stale_dt_holder(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(stale_dt_holder.valid());
  const hid_t stale_dt_id = stale_dt_holder.get();
  stale_dt_holder.reset();

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5I_is_file_object(H5I_INVALID_HID), 0);
    EXPECT_LT(H5I_is_file_object(stale_dt_id), 0);
  }

  // H5I_find_id across H5I_GROUP (VOL unwrap), H5I_DATATYPE (H5T unwrap),
  // H5I_DATASPACE (direct pointer), and user type.
  hid_t found_id = H5I_INVALID_HID;
  void* grp_obj = H5VL_object(group.get());
  ASSERT_NE(grp_obj, nullptr);
  ASSERT_GE(H5I_find_id(grp_obj, H5I_GROUP, &found_id), 0);
  EXPECT_EQ(found_id, group.get());

  void* dt_obj = H5T_get_actual_type(
      static_cast<H5T_t*>(H5I_object(committed_dt.get())));
  ASSERT_NE(dt_obj, nullptr);
  ASSERT_GE(H5I_find_id(dt_obj, H5I_DATATYPE, &found_id), 0);
  EXPECT_EQ(found_id, committed_dt.get());

  int unowned_sentinel = 123;
  ASSERT_GE(H5I_find_id(&unowned_sentinel, H5I_GROUP, &found_id), 0);
  EXPECT_EQ(found_id, H5I_INVALID_HID);

  // User type iteration (app_ref true vs false, CONT / STOP / ERROR) & find_id.
  const int type = RegisterTypeCommonRaw(0, nullptr);
  ASSERT_GE(type, static_cast<int>(H5I_NTYPES));
  absl::Cleanup cleanup = [type] {
    if (H5I_type_info_array_g[type] != nullptr) DestroyTypeRaw(type);
  };

  struct IterState {
    int visits = 0;
    int return_val = 0;
    int stop_on = 0;
  };
  auto iter_cb = [](void* /*obj*/, hid_t /*id*/, void* udata) -> int {
    auto* st = static_cast<IterState*>(udata);
    ++st->visits;
    if (st->stop_on > 0 && st->visits == st->stop_on) {
      return st->return_val;
    }
    return 0;
  };

  // Empty registered type returns H5I_INVALID_HID and 0 visits cleanly.
  IterState empty_state{};
  ASSERT_GE(IterateRaw(type, iter_cb, &empty_state, false), 0);
  EXPECT_EQ(empty_state.visits, 0);
  ASSERT_GE(FindIdRaw(&unowned_sentinel, type, &found_id), 0);
  EXPECT_EQ(found_id, H5I_INVALID_HID);

  int v1 = 1;
  int v2 = 2;
  const hid_t id_app =
      RegisterInternalRaw(type, &v1, /*app_ref=*/true, nullptr, nullptr);
  const hid_t id_int =
      RegisterInternalRaw(type, &v2, /*app_ref=*/false, nullptr, nullptr);
  ASSERT_GE(id_app, 0);
  ASSERT_GE(id_int, 0);
  EXPECT_GT(FindIdRaw(&v2, type, &found_id), -1);
  EXPECT_EQ(found_id, id_int);

  IterState app_only{};
  ASSERT_GE(IterateRaw(type, iter_cb, &app_only, /*app_ref=*/true), 0);
  EXPECT_EQ(app_only.visits, 1);

  IterState all_ids{};
  ASSERT_GE(IterateRaw(type, iter_cb, &all_ids, /*app_ref=*/false), 0);
  EXPECT_EQ(all_ids.visits, 2);

  IterState stop_early{0, 1, 1};
  ASSERT_GE(IterateRaw(type, iter_cb, &stop_early, /*app_ref=*/false), 0);
  EXPECT_EQ(stop_early.visits, 1);

  const int gone_type = RegisterTypeCommonRaw(0, nullptr);
  ASSERT_GE(gone_type, static_cast<int>(H5I_NTYPES));
  ASSERT_GE(DestroyTypeRaw(gone_type), 0);

  ScopedErrorSilence silence;
  IterState err_state{0, -1, 1};
  EXPECT_LT(IterateRaw(type, iter_cb, &err_state, /*app_ref=*/false), 0);
  EXPECT_LT(H5I_iterate(H5I_BADID, iter_cb, &all_ids, false), 0);
  EXPECT_LT(IterateRaw(999, iter_cb, &all_ids, false), 0);

  EXPECT_LT(FindIdRaw(&v1, gone_type, &found_id), 0);
  EXPECT_EQ(found_id, H5I_INVALID_HID);
  EXPECT_LT(H5I_is_file_object(id_app), 0);
}

TEST(H5IintTest, TermPackageCleansEmptyTypeInfoOnCloseAndReopens) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_hdf5 = [] { H5open(); };

  // When HDF5 is active, types with open hash tables report in_use > 0.
  EXPECT_GT(H5I_term_package(), 0);

  // Reserve and free one user slot, then attach a non-APPLICATION static class
  // with hash_table == nullptr so H5close() -> H5_term_library() ->
  // H5I_term_package() executes the empty type_info cleanup branch cleanly.
  const int slot = RegisterTypeCommonRaw(0, nullptr);
  ASSERT_GE(slot, static_cast<int>(H5I_NTYPES));
  ASSERT_GE(DestroyTypeRaw(slot), 0);

  H5I_class_t empty_cls{};
  InitClassTypeRaw(&empty_cls, slot);
  ASSERT_GE(H5I_register_type(&empty_cls), 0);
  absl::Cleanup fallback_destroy = [slot] {
    if (H5I_type_info_array_g[slot] != nullptr) {
      DestroyTypeRaw(slot);
    }
  };

  ASSERT_GE(H5close(), 0);
  EXPECT_EQ(H5I_type_info_array_g[slot], nullptr);
  EXPECT_EQ(H5I_term_package(), 0);

  ASSERT_GE(H5open(), 0);
}

}  // namespace
}  // namespace hdf5
