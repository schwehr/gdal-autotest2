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
// Tests public HDF5 ID management APIs (third_party/hdf5/src/H5I.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5I.c

#include <array>
#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5I_PUBLIC_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5I_FRIEND
#include "third_party/hdf5/src/H5Ipkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5I_PUBLIC_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5I_PUBLIC_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

#if defined(__clang__)
#define H5I_TEST_NO_SANITIZE_ENUM __attribute__((no_sanitize("enum")))
#else
#define H5I_TEST_NO_SANITIZE_ENUM
#endif

// H5I_type_t is defined without a fixed underlying type (enumerators [-2, 17]),
// so C++ -fsanitize=enum traps on dynamic types >= 32 or out-of-range test
// inputs (e.g. 999). Keep those values in int outside these annotated shims.
H5I_TEST_NO_SANITIZE_ENUM int RegisterType2Raw(unsigned reserved,
                                                H5I_free_t free_func) {
  return static_cast<int>(H5Iregister_type2(reserved, free_func));
}

H5I_TEST_NO_SANITIZE_ENUM htri_t TypeExistsRaw(int type) {
  return H5Itype_exists(static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM herr_t NmembersRaw(int type, hsize_t* num_members) {
  return H5Inmembers(static_cast<H5I_type_t>(type), num_members);
}

H5I_TEST_NO_SANITIZE_ENUM herr_t ClearTypeRaw(int type, bool force) {
  return H5Iclear_type(static_cast<H5I_type_t>(type), force);
}

H5I_TEST_NO_SANITIZE_ENUM herr_t DestroyTypeRaw(int type) {
  return H5Idestroy_type(static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM hid_t RegisterRaw(int type, const void* obj) {
  return H5Iregister(static_cast<H5I_type_t>(type), obj);
}

H5I_TEST_NO_SANITIZE_ENUM hid_t RegisterInternalRaw(int type, const void* obj,
                                                    bool app_ref) {
  return H5I__register(static_cast<H5I_type_t>(type), obj, app_ref, nullptr,
                       nullptr);
}

H5I_TEST_NO_SANITIZE_ENUM hid_t RegisterFutureRaw(
    int type, const void* obj, H5I_future_realize_func_t realize_cb,
    H5I_future_discard_func_t discard_cb) {
  return H5Iregister_future(static_cast<H5I_type_t>(type), obj, realize_cb,
                            discard_cb);
}

H5I_TEST_NO_SANITIZE_ENUM void* ObjectVerifyRaw(hid_t id, int type) {
  return H5Iobject_verify(id, static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM int GetTypeRaw(hid_t id) {
  return static_cast<int>(H5Iget_type(id));
}

H5I_TEST_NO_SANITIZE_ENUM void* RemoveVerifyRaw(hid_t id, int type) {
  return H5Iremove_verify(id, static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM int IncTypeRefRaw(int type) {
  return H5Iinc_type_ref(static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM herr_t DecTypeRefRaw(int type) {
  return H5Idec_type_ref(static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM int GetTypeRefRaw(int type) {
  return H5Iget_type_ref(static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM void* SearchRaw(int type, H5I_search_func_t func,
                                          void* key) {
  return H5Isearch(static_cast<H5I_type_t>(type), func, key);
}

H5I_TEST_NO_SANITIZE_ENUM herr_t IterateRaw(int type, H5I_iterate_func_t op,
                                            void* op_data) {
  return H5Iiterate(static_cast<H5I_type_t>(type), op, op_data);
}

struct FreeState {
  std::vector<int> freed_tags;
  bool should_fail = false;
};

struct TaggedObject {
  FreeState* state = nullptr;
  int tag = 0;
};

herr_t FreeTaggedObject(void* obj, void** /*request*/) {
  auto* tagged = static_cast<TaggedObject*>(obj);
  if (tagged->state->should_fail) {
    return -1;
  }
  tagged->state->freed_tags.push_back(tagged->tag);
  return 0;
}

struct FuturePayload {
  int target_type = static_cast<int>(H5I_BADID);
  TaggedObject* actual_obj = nullptr;
  int discard_calls = 0;
};

herr_t RealizeFuturePayload(void* future_obj, hid_t* actual_id) {
  auto* payload = static_cast<FuturePayload*>(future_obj);
  *actual_id = RegisterRaw(payload->target_type, payload->actual_obj);
  return (*actual_id >= 0) ? 0 : -1;
}

herr_t DiscardFuturePayload(void* future_obj) {
  auto* payload = static_cast<FuturePayload*>(future_obj);
  ++payload->discard_calls;
  return 0;
}

herr_t NoOpCloseGroup(void* /*grp*/, hid_t /*dxpl_id*/, void** /*req*/) {
  return 0;
}

TEST(H5ITest, TypeLifecycleAndBoundaryValidation) {
  FreeState free_state;
  TaggedObject obj_keep{&free_state, 1};
  TaggedObject obj_drop{&free_state, 2};

  const int custom_type =
      RegisterType2Raw(/*reserved=*/2, FreeTaggedObject);
  ASSERT_GE(custom_type, static_cast<int>(H5I_NTYPES));
  absl::Cleanup destroy_custom = [custom_type, &free_state] {
    free_state.should_fail = false;
    if (TypeExistsRaw(custom_type) > 0) {
      DestroyTypeRaw(custom_type);
    }
  };

  EXPECT_GT(TypeExistsRaw(custom_type), 0);

  // num_members == nullptr is permitted.
  EXPECT_GE(NmembersRaw(custom_type, nullptr), 0);

  hsize_t count = 99;
  ASSERT_GE(NmembersRaw(custom_type, &count), 0);
  EXPECT_EQ(count, 0);

  const hid_t id_keep = RegisterRaw(custom_type, &obj_keep);
  const hid_t id_drop = RegisterRaw(custom_type, &obj_drop);
  ASSERT_GE(id_keep, 0);
  ASSERT_GE(id_drop, 0);
  EXPECT_EQ(H5Iinc_ref(id_keep), 2);

  ASSERT_GE(NmembersRaw(custom_type, &count), 0);
  EXPECT_EQ(count, 2);

  // force = false clears only refcount == 1 IDs.
  ASSERT_GE(ClearTypeRaw(custom_type, /*force=*/false), 0);
  ASSERT_EQ(free_state.freed_tags.size(), 1);
  EXPECT_EQ(free_state.freed_tags[0], 2);
  EXPECT_GT(H5Iis_valid(id_keep), 0);
  EXPECT_EQ(H5Iis_valid(id_drop), 0);

  // force = true removes even refcount > 1 IDs.
  ASSERT_GE(ClearTypeRaw(custom_type, /*force=*/true), 0);
  ASSERT_EQ(free_state.freed_tags.size(), 2);
  EXPECT_EQ(free_state.freed_tags[1], 1);
  ASSERT_GE(NmembersRaw(custom_type, &count), 0);
  EXPECT_EQ(count, 0);

  // Destroyed type checks.
  ASSERT_GE(DestroyTypeRaw(custom_type), 0);
  EXPECT_EQ(TypeExistsRaw(custom_type), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Itype_exists(H5I_FILE), 0);
  EXPECT_LT(H5Itype_exists(H5I_UNINIT), 0);
  EXPECT_LT(TypeExistsRaw(999), 0);

  EXPECT_LT(H5Inmembers(H5I_GROUP, &count), 0);
  EXPECT_LT(H5Inmembers(H5I_BADID, &count), 0);
  EXPECT_LT(NmembersRaw(999, &count), 0);
  EXPECT_LT(NmembersRaw(custom_type, &count), 0);

  EXPECT_LT(H5Iclear_type(H5I_GROUP, false), 0);
  EXPECT_LT(ClearTypeRaw(custom_type, false), 0);
  EXPECT_LT(ClearTypeRaw(999, false), 0);

  EXPECT_LT(H5Idestroy_type(H5I_GROUP), 0);
  EXPECT_LT(DestroyTypeRaw(custom_type), 0);
  EXPECT_LT(DestroyTypeRaw(999), 0);
}

TEST(H5ITest, RegisterTypeExhaustionFails) {
  std::vector<int> allocated;
  absl::Cleanup cleanup = [&allocated] {
    for (const int type : allocated) {
      DestroyTypeRaw(type);
    }
  };

  ScopedErrorSilence silence;
  while (true) {
    const int t = RegisterType2Raw(0, nullptr);
    if (t == static_cast<int>(H5I_BADID)) {
      break;
    }
    allocated.push_back(t);
    ASSERT_LE(allocated.size(), 256);
  }
  EXPECT_FALSE(allocated.empty());
  EXPECT_EQ(RegisterType2Raw(0, nullptr), static_cast<int>(H5I_BADID));
}

TEST(H5ITest, RegisterObjectVerifyGetTypeRemoveAndIsValid) {
  FreeState state;
  TaggedObject a{&state, 100};
  TaggedObject b{&state, 200};

  const int type1 = RegisterType2Raw(0, FreeTaggedObject);
  const int type2 = RegisterType2Raw(0, nullptr);
  ASSERT_GE(type1, static_cast<int>(H5I_NTYPES));
  ASSERT_GE(type2, static_cast<int>(H5I_NTYPES));
  absl::Cleanup cleanup = [type1, type2] {
    if (TypeExistsRaw(type1) > 0) DestroyTypeRaw(type1);
    if (TypeExistsRaw(type2) > 0) DestroyTypeRaw(type2);
  };

  const hid_t id1 = RegisterRaw(type1, &a);
  const hid_t id2 = RegisterRaw(type1, &a);
  const hid_t id3 = RegisterRaw(type2, &b);
  ASSERT_GE(id1, 0);
  ASSERT_GE(id2, 0);
  ASSERT_NE(id1, id2);
  ASSERT_GE(id3, 0);

  EXPECT_EQ(ObjectVerifyRaw(id1, type1), &a);
  EXPECT_EQ(ObjectVerifyRaw(id3, type2), &b);
  EXPECT_EQ(GetTypeRaw(id1), type1);
  EXPECT_EQ(GetTypeRaw(id3), type2);

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  EXPECT_EQ(H5Iget_type(space.get()), H5I_DATASPACE);
  EXPECT_GT(H5Iis_valid(space.get()), 0);
  EXPECT_GT(H5Iis_valid(id1), 0);

  // H5Iis_valid returns false for internal-only IDs (app_count == 0).
  const hid_t internal_id = RegisterInternalRaw(type2, &b, /*app_ref=*/false);
  ASSERT_GE(internal_id, 0);
  EXPECT_EQ(H5Iis_valid(internal_id), 0);
  EXPECT_EQ(RemoveVerifyRaw(internal_id, type2), &b);

  // H5Iremove_verify removes without calling free_func.
  EXPECT_EQ(RemoveVerifyRaw(id1, type2), nullptr);
  EXPECT_EQ(RemoveVerifyRaw(id1, type1), &a);
  EXPECT_TRUE(state.freed_tags.empty());
  EXPECT_EQ(GetTypeRaw(id1), static_cast<int>(H5I_BADID));
  EXPECT_EQ(H5Iis_valid(id1), 0);
  EXPECT_EQ(H5Iis_valid(H5I_INVALID_HID), 0);

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Iregister(H5I_GROUP, &a), H5I_INVALID_HID);
  EXPECT_EQ(RegisterRaw(999, &a), H5I_INVALID_HID);

  EXPECT_EQ(H5Iobject_verify(id2, H5I_FILE), nullptr);
  EXPECT_EQ(H5Iobject_verify(id2, H5I_BADID), nullptr);
  EXPECT_EQ(ObjectVerifyRaw(id2, 999), nullptr);
  EXPECT_EQ(ObjectVerifyRaw(id2, type2), nullptr);

  EXPECT_EQ(H5Iremove_verify(id2, H5I_FILE), nullptr);
  EXPECT_EQ(H5Iget_type(H5I_INVALID_HID), H5I_BADID);
  const hid_t out_of_range_id = (static_cast<hid_t>(126) << ID_BITS) | 1;
  EXPECT_EQ(GetTypeRaw(out_of_range_id), static_cast<int>(H5I_BADID));
}

TEST(H5ITest, FutureObjectRegistrationAndErrorPaths) {
  FreeState state;
  TaggedObject actual{&state, 77};

  const int type = RegisterType2Raw(0, FreeTaggedObject);
  ASSERT_GE(type, static_cast<int>(H5I_NTYPES));
  absl::Cleanup destroy = [type] {
    if (TypeExistsRaw(type) > 0) DestroyTypeRaw(type);
  };

  FuturePayload payload{type, &actual, 0};
  const hid_t future_id = RegisterFutureRaw(type, &payload, RealizeFuturePayload,
                                            DiscardFuturePayload);
  ASSERT_GE(future_id, 0);
  EXPECT_GT(H5Iis_valid(future_id), 0);

  EXPECT_EQ(ObjectVerifyRaw(future_id, type), &actual);
  EXPECT_EQ(payload.discard_calls, 1);
  EXPECT_EQ(H5Idec_ref(future_id), 0);
  ASSERT_EQ(state.freed_tags.size(), 1);
  EXPECT_EQ(state.freed_tags[0], 77);

  ScopedErrorSilence silence;
  EXPECT_EQ(RegisterFutureRaw(type, &payload, nullptr, DiscardFuturePayload),
            H5I_INVALID_HID);
  EXPECT_EQ(RegisterFutureRaw(type, &payload, RealizeFuturePayload, nullptr),
            H5I_INVALID_HID);
  EXPECT_EQ(
      RegisterFutureRaw(999, &payload, RealizeFuturePayload,
                        DiscardFuturePayload),
      H5I_INVALID_HID);
}

TEST(H5ITest, RefCountingAndTypeRefCounting) {
  FreeState state;
  TaggedObject obj{&state, 55};

  const int type = RegisterType2Raw(0, FreeTaggedObject);
  ASSERT_GE(type, static_cast<int>(H5I_NTYPES));
  absl::Cleanup destroy = [type, &state] {
    state.should_fail = false;
    if (TypeExistsRaw(type) > 0) {
      DecTypeRefRaw(type);
      if (TypeExistsRaw(type) > 0) {
        DestroyTypeRaw(type);
      }
    }
  };

  // Object reference counting.
  const hid_t id = RegisterRaw(type, &obj);
  ASSERT_GE(id, 0);
  EXPECT_EQ(H5Iget_ref(id), 1);
  EXPECT_EQ(H5Iinc_ref(id), 2);
  EXPECT_EQ(H5Iget_ref(id), 2);
  EXPECT_EQ(H5Idec_ref(id), 1);

  {
    ScopedErrorSilence silence;
    state.should_fail = true;
    EXPECT_LT(H5Idec_ref(id), 0);
    EXPECT_EQ(H5Iget_ref(id), 1);
    state.should_fail = false;
  }

  EXPECT_EQ(H5Idec_ref(id), 0);
  ASSERT_EQ(state.freed_tags.size(), 1);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Iinc_ref(H5I_INVALID_HID), 0);
    EXPECT_LT(H5Idec_ref(H5I_INVALID_HID), 0);
    EXPECT_LT(H5Iget_ref(H5I_INVALID_HID), 0);
    EXPECT_LT(H5Iinc_ref(id), 0);
    EXPECT_LT(H5Idec_ref(id), 0);
    EXPECT_LT(H5Iget_ref(id), 0);
  }

  // Type reference counting.
  EXPECT_EQ(GetTypeRefRaw(type), 1);
  EXPECT_EQ(IncTypeRefRaw(type), 2);
  EXPECT_EQ(GetTypeRefRaw(type), 2);
  EXPECT_EQ(DecTypeRefRaw(type), 1);
  EXPECT_EQ(DecTypeRefRaw(type), 0);
  EXPECT_EQ(TypeExistsRaw(type), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Iinc_type_ref(H5I_FILE), 0);
  EXPECT_LT(H5Idec_type_ref(H5I_FILE), 0);
  EXPECT_LT(H5Iget_type_ref(H5I_FILE), 0);

  EXPECT_LT(H5Iinc_type_ref(H5I_BADID), 0);
  EXPECT_LT(IncTypeRefRaw(999), 0);
  EXPECT_LT(H5Iget_type_ref(H5I_BADID), 0);
  EXPECT_LT(GetTypeRefRaw(999), 0);
  EXPECT_LT(IncTypeRefRaw(type), 0);
  EXPECT_LT(DecTypeRefRaw(type), 0);
  EXPECT_LT(GetTypeRefRaw(type), 0);
}

TEST(H5ITest, SearchAndIterateCallbacks) {
  const int type = RegisterType2Raw(0, nullptr);
  ASSERT_GE(type, static_cast<int>(H5I_NTYPES));
  absl::Cleanup destroy = [type] { DestroyTypeRaw(type); };

  int v1 = 10;
  int v2 = 20;
  int v3 = 30;
  ASSERT_GE(RegisterRaw(type, &v1), 0);
  ASSERT_GE(RegisterRaw(type, &v2), 0);
  ASSERT_GE(RegisterRaw(type, &v3), 0);

  struct SearchCtx {
    int target = 0;
    int error_on = -1;
    int visited = 0;
  };
  auto search_cb = [](void* obj, hid_t /*id*/, void* key) -> int {
    auto* ctx = static_cast<SearchCtx*>(key);
    ++ctx->visited;
    const int val = *static_cast<int*>(obj);
    if (val == ctx->error_on) return -1;
    if (val == ctx->target) return 1;
    return 0;
  };

  // Note: upstream H5I__search_cb initializes ret_value to H5_ITER_ERROR and
  // leaves it at H5_ITER_ERROR when app_cb returns 0, so H5Isearch terminates
  // after the first non-matching item with nullptr.
  SearchCtx no_match{999, -1, 0};
  EXPECT_EQ(SearchRaw(type, search_cb, &no_match), nullptr);
  EXPECT_EQ(no_match.visited, 1);

  // Use a single-entry type so the first visited object matches (cb_ret_val
  // > 0 -> H5_ITER_STOP).
  const int hit_type = RegisterType2Raw(0, nullptr);
  ASSERT_GE(hit_type, static_cast<int>(H5I_NTYPES));
  absl::Cleanup destroy_hit = [hit_type] { DestroyTypeRaw(hit_type); };
  ASSERT_GE(RegisterRaw(hit_type, &v2), 0);

  SearchCtx match_v2{20, -1, 0};
  EXPECT_EQ(SearchRaw(hit_type, search_cb, &match_v2), &v2);
  EXPECT_EQ(match_v2.visited, 1);

  {
    ScopedErrorSilence silence;
    SearchCtx err_ctx{-1, 10, 0};
    EXPECT_EQ(SearchRaw(type, search_cb, &err_ctx), nullptr);
    EXPECT_EQ(H5Isearch(H5I_GROUP, search_cb, &err_ctx), nullptr);
  }

  struct IterCtx {
    int stop_after = 0;
    int error_at = 0;
    int visited = 0;
  };
  auto iter_cb = [](hid_t /*id*/, void* op_data) -> herr_t {
    auto* ctx = static_cast<IterCtx*>(op_data);
    ++ctx->visited;
    if (ctx->error_at > 0 && ctx->visited == ctx->error_at) return -1;
    if (ctx->stop_after > 0 && ctx->visited == ctx->stop_after) return 7;
    return 0;
  };

  IterCtx cont_ctx{0, 0, 0};
  EXPECT_GE(IterateRaw(type, iter_cb, &cont_ctx), 0);
  EXPECT_EQ(cont_ctx.visited, 3);

  IterCtx stop_ctx{1, 0, 0};
  EXPECT_GE(IterateRaw(type, iter_cb, &stop_ctx), 0);
  EXPECT_EQ(stop_ctx.visited, 1);

  ScopedErrorSilence silence;
  IterCtx err_iter{0, 1, 0};
  EXPECT_LT(IterateRaw(type, iter_cb, &err_iter), 0);
  EXPECT_LT(IterateRaw(999, iter_cb, &err_iter), 0);
}

TEST(H5ITest, GetFileIdAndGetNameForObjectsAndErrors) {
  ScopedFile file = CreateInMemoryFile("h5i_get_name.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup group(H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
  ASSERT_TRUE(group.valid());

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());

  ScopedDataset dset(H5Dcreate2(group.get(), "dset", H5T_NATIVE_INT,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  ScopedAttr attr(H5Acreate2(dset.get(), "attr", H5T_NATIVE_INT, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());

  ScopedType committed_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(committed_dt.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/dt", committed_dt.get(), H5P_DEFAULT,
                      H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedType transient_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(transient_dt.valid());

  // H5Iget_file_id works for file, group, dataset, committed datatype, attr.
  for (const hid_t obj_id :
       {file.get(), group.get(), dset.get(), committed_dt.get(), attr.get()}) {
    ScopedFile retrieved_file(H5Iget_file_id(obj_id));
    EXPECT_TRUE(retrieved_file.valid());
    EXPECT_EQ(H5Iget_type(retrieved_file.get()), H5I_FILE);
  }

  // H5Iget_name: full name, length query, zero-size non-null buffer guard,
  // truncation, attribute parent path, and root file path.
  std::array<char, 32> name_buf{};
  EXPECT_EQ(H5Iget_name(dset.get(), name_buf.data(), name_buf.size()), 9);
  EXPECT_STREQ(name_buf.data(), "/grp/dset");

  EXPECT_EQ(H5Iget_name(attr.get(), nullptr, 0), 9);

  std::array<char, 8> sentinel_buf{};
  sentinel_buf.fill('Z');
  EXPECT_EQ(H5Iget_name(dset.get(), sentinel_buf.data(), 0), 9);
  for (const char ch : sentinel_buf) {
    EXPECT_EQ(ch, 'Z');
  }

  std::array<char, 5> trunc_buf{};
  EXPECT_EQ(H5Iget_name(dset.get(), trunc_buf.data(), trunc_buf.size()), 9);
  EXPECT_STREQ(trunc_buf.data(), "/grp");

  EXPECT_EQ(H5Iget_name(file.get(), name_buf.data(), name_buf.size()), 1);
  EXPECT_STREQ(name_buf.data(), "/");

  // Re-opening file ID after closing initial file handle while child is open.
  file.reset();
  {
    ScopedFile reopened(H5Iget_file_id(group.get()));
    EXPECT_TRUE(reopened.valid());
  }

  // Error paths on non-file IDs, closed IDs, and VOL get failure.
  ScopedGroup closed_grp(group.release());
  const hid_t stale_grp_id = closed_grp.get();
  closed_grp.reset();

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Iget_file_id(H5I_INVALID_HID), H5I_INVALID_HID);
  EXPECT_EQ(H5Iget_file_id(space.get()), H5I_INVALID_HID);
  EXPECT_EQ(H5Iget_file_id(transient_dt.get()), H5I_INVALID_HID);
  EXPECT_EQ(H5Iget_file_id(stale_grp_id), H5I_INVALID_HID);

  EXPECT_LT(H5Iget_name(H5I_INVALID_HID, name_buf.data(), name_buf.size()), 0);
  EXPECT_LT(H5Iget_name(space.get(), name_buf.data(), name_buf.size()), 0);
  EXPECT_LT(H5Iget_name(transient_dt.get(), name_buf.data(), name_buf.size()),
            0);
  EXPECT_LT(H5Iget_name(stale_grp_id, name_buf.data(), name_buf.size()), 0);

  // Minimal VOL connector lacking object_cls.get and file_cls.get exercises
  // H5VL_object_get and H5F_get_file_id failure branches cleanly.
  H5VL_class_t bare_cls{};
  bare_cls.version = H5VL_VERSION;
  bare_cls.value = static_cast<H5VL_class_value_t>(H5_VOL_RESERVED + 21);
  bare_cls.name = "h5i_public_bare_vol";
  bare_cls.group_cls.close = NoOpCloseGroup;
  const hid_t conn_id = H5VLregister_connector(&bare_cls, H5P_DEFAULT);
  ASSERT_GE(conn_id, 0);
  absl::Cleanup unreg = [conn_id] { H5VLunregister_connector(conn_id); };
  auto* conn =
      static_cast<H5VL_connector_t*>(H5I_object_verify(conn_id, H5I_VOL));
  ASSERT_NE(conn, nullptr);

  int dummy_grp_data = 1;
  ScopedGroup bare_grp(
      H5VL_register(H5I_GROUP, &dummy_grp_data, conn, /*app_ref=*/true));
  ASSERT_TRUE(bare_grp.valid());
  EXPECT_LT(H5Iget_name(bare_grp.get(), name_buf.data(), name_buf.size()), 0);
  EXPECT_EQ(H5Iget_file_id(bare_grp.get()), H5I_INVALID_HID);
}

}  // namespace
}  // namespace hdf5
