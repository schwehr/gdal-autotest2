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
// Tests deprecated property list APIs (third_party/hdf5/src/H5Pdeprec.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pdeprec.c

#include "third_party/hdf5/src/hdf5.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ScopedPlistClass = ScopedH5Id<H5Pclose_class>;

struct CallbackCounts {
  std::atomic<int> create{0};
  std::atomic<int> set{0};
  std::atomic<int> get{0};
  std::atomic<int> del{0};
  std::atomic<int> copy{0};
  std::atomic<int> close{0};
};

CallbackCounts g_cb_counts;

herr_t PropCreateCb(const char* /*name*/, size_t size, void* value) {
  ++g_cb_counts.create;
  if (size == sizeof(int32_t) && value != nullptr) {
    *static_cast<int32_t*>(value) += 1;
  }
  return 0;
}

herr_t PropSetCb(hid_t /*prop_id*/, const char* /*name*/, size_t size,
                 void* value) {
  ++g_cb_counts.set;
  if (size == sizeof(int32_t) && value != nullptr) {
    *static_cast<int32_t*>(value) += 2;
  }
  return 0;
}

herr_t PropGetCb(hid_t /*prop_id*/, const char* /*name*/, size_t size,
                 void* value) {
  ++g_cb_counts.get;
  if (size == sizeof(int32_t) && value != nullptr) {
    *static_cast<int32_t*>(value) += 3;
  }
  return 0;
}

herr_t PropDeleteCb(hid_t /*prop_id*/, const char* /*name*/, size_t /*size*/,
                    void* /*value*/) {
  ++g_cb_counts.del;
  return 0;
}

herr_t PropCopyCb(const char* /*name*/, size_t size, void* value) {
  ++g_cb_counts.copy;
  if (size == sizeof(int32_t) && value != nullptr) {
    *static_cast<int32_t*>(value) += 10;
  }
  return 0;
}

herr_t PropCloseCb(const char* /*name*/, size_t /*size*/, void* /*value*/) {
  ++g_cb_counts.close;
  return 0;
}

TEST(H5PdeprecTest, Register1RejectsInvalidArguments) {
  ScopedPlistClass cls(H5Pcreate_class(H5P_ROOT, "deprec_err_cls", nullptr,
                                       nullptr, nullptr, nullptr, nullptr,
                                       nullptr));
  ASSERT_GE(cls.get(), 0);
  ScopedPlist plist(H5Pcreate(cls.get()));
  ASSERT_GE(plist.get(), 0);

  int32_t def_val = 7;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pregister1(H5I_INVALID_HID, "p", sizeof(def_val), &def_val,
                           nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr),
              0);
    EXPECT_LT(H5Pregister1(plist.get(), "p", sizeof(def_val), &def_val, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pregister1(cls.get(), nullptr, sizeof(def_val), &def_val,
                           nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr),
              0);
    EXPECT_LT(H5Pregister1(cls.get(), "", sizeof(def_val), &def_val, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pregister1(cls.get(), "p", sizeof(def_val), nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
  }

  ASSERT_GE(H5Pregister1(cls.get(), "dup_prop", sizeof(def_val), &def_val,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pregister1(cls.get(), "dup_prop", sizeof(def_val), &def_val,
                           nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr),
              0);
  }
}

TEST(H5PdeprecTest, Register1CallbacksZeroSizeAndClassSplit) {
  ScopedPlistClass cls(H5Pcreate_class(H5P_ROOT, "deprec_split_cls", nullptr,
                                       nullptr, nullptr, nullptr, nullptr,
                                       nullptr));
  ASSERT_GE(cls.get(), 0);

  int32_t def_val = 10;
  ASSERT_GE(H5Pregister1(cls.get(), "cb_prop", sizeof(def_val), &def_val,
                         PropCreateCb, PropSetCb, PropGetCb, PropDeleteCb,
                         PropCopyCb, PropCloseCb),
            0);
  ASSERT_GE(H5Pregister1(cls.get(), "flag_prop", 0, nullptr, PropCreateCb,
                         nullptr, nullptr, nullptr, nullptr, PropCloseCb),
            0);

  const int create_before = g_cb_counts.create.load();
  const int set_before = g_cb_counts.set.load();
  const int get_before = g_cb_counts.get.load();
  const int copy_before = g_cb_counts.copy.load();
  const int del_before = g_cb_counts.del.load();
  const int close_before = g_cb_counts.close.load();

  ScopedPlist p_before(H5Pcreate(cls.get()));
  ASSERT_GE(p_before.get(), 0);
  // Creating p_before invoked PropCreateCb for both cb_prop and flag_prop.
  EXPECT_EQ(g_cb_counts.create.load() - create_before, 2);
  EXPECT_GT(H5Pexist(p_before.get(), "flag_prop"), 0);

  int32_t val = 0;
  ASSERT_GE(H5Pget(p_before.get(), "cb_prop", &val), 0);
  // 10 + 1 (create) + 3 (get).
  EXPECT_EQ(val, 14);
  EXPECT_EQ(g_cb_counts.get.load() - get_before, 1);

  int32_t new_val = 20;
  ASSERT_GE(H5Pset(p_before.get(), "cb_prop", &new_val), 0);
  EXPECT_EQ(g_cb_counts.set.load() - set_before, 1);
  // Overwriting via H5Pset invokes delete on the prior value once.
  EXPECT_EQ(g_cb_counts.del.load() - del_before, 1);

  ASSERT_GE(H5Pget(p_before.get(), "cb_prop", &val), 0);
  // 20 + 2 (set) + 3 (get).
  EXPECT_EQ(val, 25);

  // Register another property while p_before is open to trigger class splitting
  // inside H5Pregister1 (pclass != orig_pclass -> H5I_subst + H5P__close_class).
  int32_t split_def = 99;
  ASSERT_GE(H5Pregister1(cls.get(), "split_prop", sizeof(split_def), &split_def,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
            0);
  EXPECT_EQ(H5Pexist(p_before.get(), "split_prop"), 0);

  ScopedPlist p_after(H5Pcreate(cls.get()));
  ASSERT_GE(p_after.get(), 0);
  EXPECT_GT(H5Pexist(p_after.get(), "split_prop"), 0);

  {
    ScopedPlist p_copy(H5Pcopy(p_before.get()));
    ASSERT_GE(p_copy.get(), 0);
    EXPECT_EQ(g_cb_counts.copy.load() - copy_before, 1);
    int32_t copied_val = 0;
    ASSERT_GE(H5Pget(p_copy.get(), "cb_prop", &copied_val), 0);
    // Stored was 22; +10 (copy) + 3 (get).
    EXPECT_EQ(copied_val, 35);
    ASSERT_GE(H5Premove(p_copy.get(), "cb_prop"), 0);
    EXPECT_EQ(g_cb_counts.del.load() - del_before, 2);
  }
  EXPECT_GE(g_cb_counts.close.load() - close_before, 1);
}

TEST(H5PdeprecTest, Insert1RejectsInvalidArgsAndExercisesCallbacks) {
  ScopedPlist plist(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(plist.get(), 0);

  int32_t init_val = 5;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pinsert1(H5I_INVALID_HID, "x", sizeof(init_val), &init_val,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pinsert1(H5P_DEFAULT, "x", sizeof(init_val), &init_val, nullptr,
                         nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pinsert1(H5P_DATASET_CREATE, "x", sizeof(init_val), &init_val,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pinsert1(plist.get(), nullptr, sizeof(init_val), &init_val,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pinsert1(plist.get(), "", sizeof(init_val), &init_val, nullptr,
                         nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pinsert1(plist.get(), "x", sizeof(init_val), nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr),
              0);
  }

  const int set_before = g_cb_counts.set.load();
  const int get_before = g_cb_counts.get.load();
  const int copy_before = g_cb_counts.copy.load();
  const int del_before = g_cb_counts.del.load();
  const int close_before = g_cb_counts.close.load();

  ASSERT_GE(H5Pinsert1(plist.get(), "temp_int", sizeof(init_val), &init_val,
                       PropSetCb, PropGetCb, PropDeleteCb, PropCopyCb,
                       PropCloseCb),
            0);
  // H5Pinsert1 stores the initial value without invoking the set callback.
  EXPECT_EQ(g_cb_counts.set.load() - set_before, 0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pinsert1(plist.get(), "temp_int", sizeof(init_val), &init_val,
                         nullptr, nullptr, nullptr, nullptr, nullptr),
              0);
  }

  ASSERT_GE(H5Pinsert1(plist.get(), "temp_zero", 0, nullptr, nullptr, nullptr,
                       nullptr, nullptr, PropCloseCb),
            0);
  EXPECT_GT(H5Pexist(plist.get(), "temp_zero"), 0);

  int32_t out_val = 0;
  ASSERT_GE(H5Pget(plist.get(), "temp_int", &out_val), 0);
  // 5 (initial) + 3 (get).
  EXPECT_EQ(out_val, 8);
  EXPECT_EQ(g_cb_counts.get.load() - get_before, 1);

  int32_t updated_val = 20;
  ASSERT_GE(H5Pset(plist.get(), "temp_int", &updated_val), 0);
  EXPECT_EQ(g_cb_counts.set.load() - set_before, 1);
  ASSERT_GE(H5Pget(plist.get(), "temp_int", &out_val), 0);
  // 20 + 2 (set) + 3 (get).
  EXPECT_EQ(out_val, 25);

  {
    ScopedPlist copy(H5Pcopy(plist.get()));
    ASSERT_GE(copy.get(), 0);
    EXPECT_EQ(g_cb_counts.copy.load() - copy_before, 1);
    ASSERT_GE(H5Premove(copy.get(), "temp_int"), 0);
    // Once from H5Pset overwrite + once from H5Premove.
    EXPECT_EQ(g_cb_counts.del.load() - del_before, 2);
  }
  EXPECT_GE(g_cb_counts.close.load() - close_before, 1);
}

TEST(H5PdeprecTest, GetVersionDefaultLatestAndNullOutputs) {
  {
    ScopedErrorSilence silence;
    unsigned super = 0;
    EXPECT_LT(H5Pget_version(H5I_INVALID_HID, &super, nullptr, nullptr, nullptr),
              0);
    EXPECT_LT(H5Pget_version(H5P_DEFAULT, &super, nullptr, nullptr, nullptr),
              0);
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_GE(dxpl.get(), 0);
    EXPECT_LT(H5Pget_version(dxpl.get(), &super, nullptr, nullptr, nullptr), 0);
  }

  unsigned super = 99;
  unsigned freelist = 99;
  unsigned stab = 99;
  unsigned shhdr = 99;
  ASSERT_GE(H5Pget_version(H5P_FILE_CREATE_DEFAULT, &super, &freelist, &stab,
                           &shhdr),
            0);
  EXPECT_EQ(super, 0u);
  EXPECT_EQ(freelist, 0u);
  EXPECT_EQ(stab, 0u);
  EXPECT_EQ(shhdr, 0u);

  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(fcpl.get(), 0);
  EXPECT_GE(H5Pget_version(fcpl.get(), nullptr, nullptr, nullptr, nullptr), 0);

  // Opening/creating a file with latest format updates superblock version.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);
  ScopedFile file(H5Fcreate("ver_latest.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  ASSERT_TRUE(file.valid());
  ScopedPlist actual_fcpl(H5Fget_create_plist(file.get()));
  ASSERT_TRUE(actual_fcpl.valid());
  ASSERT_GE(H5Pget_version(actual_fcpl.get(), &super, &freelist, &stab, &shhdr),
            0);
  EXPECT_GE(super, 2u);
}

TEST(H5PdeprecTest, Encode1RoundTripAndErrorPaths) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(fcpl.get(), 0);
  ASSERT_GE(H5Pset_sym_k(fcpl.get(), 8, 12), 0);

  size_t nalloc = 0;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pencode1(H5I_INVALID_HID, nullptr, &nalloc), 0);
    EXPECT_LT(H5Pencode1(H5P_DEFAULT, nullptr, &nalloc), 0);
    EXPECT_LT(H5Pencode1(H5P_FILE_CREATE, nullptr, &nalloc), 0);
    EXPECT_LT(H5Pencode1(fcpl.get(), nullptr, nullptr), 0);
  }

  ASSERT_GE(H5Pencode1(fcpl.get(), nullptr, &nalloc), 0);
  ASSERT_GT(nalloc, 0u);

  std::vector<uint8_t> buf(nalloc, 0);
  ASSERT_GE(H5Pencode1(fcpl.get(), buf.data(), &nalloc), 0);

  ScopedPlist decoded(H5Pdecode(buf.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(fcpl.get(), decoded.get()), 0);
}

TEST(H5PdeprecTest, SetAndGetFileSpaceStrategiesAndErrors) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(fcpl.get(), 0);
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(dxpl.get(), 0);

  {
    ScopedErrorSilence silence;
    H5F_file_space_type_t st = H5F_FILE_SPACE_DEFAULT;
    hsize_t thresh = 0;

    EXPECT_LT(H5Pset_file_space(fcpl.get(), H5F_FILE_SPACE_NTYPES, 1), 0);
    EXPECT_LT(H5Pset_file_space(fcpl.get(),
                                static_cast<H5F_file_space_type_t>(-1), 1),
              0);
    EXPECT_LT(H5Pset_file_space(H5P_DEFAULT, H5F_FILE_SPACE_ALL, 1), 0);
    EXPECT_LT(H5Pset_file_space(H5P_FILE_CREATE_DEFAULT, H5F_FILE_SPACE_ALL, 1),
              0);
    EXPECT_LT(H5Pset_file_space(dxpl.get(), H5F_FILE_SPACE_ALL, 1), 0);
    EXPECT_LT(H5Pset_file_space(H5I_INVALID_HID, H5F_FILE_SPACE_ALL, 1), 0);

    EXPECT_LT(H5Pget_file_space(H5I_INVALID_HID, &st, &thresh), 0);
    EXPECT_LT(H5Pget_file_space(H5P_DEFAULT, &st, &thresh), 0);
    EXPECT_LT(H5Pget_file_space(dxpl.get(), &st, &thresh), 0);
  }

  H5F_file_space_type_t strategy = H5F_FILE_SPACE_DEFAULT;
  hsize_t threshold = 0;
  ASSERT_GE(H5Pget_file_space(H5P_FILE_CREATE_DEFAULT, &strategy, &threshold),
            0);
  EXPECT_EQ(strategy, H5F_FILE_SPACE_ALL);
  EXPECT_GE(threshold, 1u);

  // Null output pointers are permitted by H5Pget_file_space.
  EXPECT_GE(H5Pget_file_space(fcpl.get(), nullptr, nullptr), 0);
  EXPECT_GE(H5Pget_file_space(fcpl.get(), &strategy, nullptr), 0);
  EXPECT_GE(H5Pget_file_space(fcpl.get(), nullptr, &threshold), 0);

  // H5F_FILE_SPACE_ALL_PERSIST -> FSM_AGGR + persist=true.
  ASSERT_GE(H5Pset_file_space(fcpl.get(), H5F_FILE_SPACE_ALL_PERSIST, 2048), 0);
  ASSERT_GE(H5Pget_file_space(fcpl.get(), &strategy, &threshold), 0);
  EXPECT_EQ(strategy, H5F_FILE_SPACE_ALL_PERSIST);
  EXPECT_EQ(threshold, 2048u);

  // H5F_FILE_SPACE_DEFAULT keeps existing strategy, updates threshold.
  ASSERT_GE(H5Pset_file_space(fcpl.get(), H5F_FILE_SPACE_DEFAULT, 4096), 0);
  ASSERT_GE(H5Pget_file_space(fcpl.get(), &strategy, &threshold), 0);
  EXPECT_EQ(strategy, H5F_FILE_SPACE_ALL_PERSIST);
  EXPECT_EQ(threshold, 4096u);

  // threshold == 0 preserves existing threshold while updating strategy.
  ASSERT_GE(H5Pset_file_space(fcpl.get(), H5F_FILE_SPACE_ALL, 0), 0);
  ASSERT_GE(H5Pget_file_space(fcpl.get(), &strategy, &threshold), 0);
  EXPECT_EQ(strategy, H5F_FILE_SPACE_ALL);
  EXPECT_EQ(threshold, 4096u);

  // H5F_FILE_SPACE_AGGR_VFD -> AGGR.
  ASSERT_GE(H5Pset_file_space(fcpl.get(), H5F_FILE_SPACE_AGGR_VFD, 512), 0);
  ASSERT_GE(H5Pget_file_space(fcpl.get(), &strategy, &threshold), 0);
  EXPECT_EQ(strategy, H5F_FILE_SPACE_AGGR_VFD);

  // H5F_FILE_SPACE_VFD -> NONE.
  ASSERT_GE(H5Pset_file_space(fcpl.get(), H5F_FILE_SPACE_VFD, 512), 0);
  ASSERT_GE(H5Pget_file_space(fcpl.get(), &strategy, &threshold), 0);
  EXPECT_EQ(strategy, H5F_FILE_SPACE_VFD);

  // PAGED strategy has no deprecated H5F_file_space_type_t equivalent.
  ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE,
                                       false, 1024),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pget_file_space(fcpl.get(), &strategy, &threshold), 0);
    EXPECT_LT(H5Pset_file_space(fcpl.get(), H5F_FILE_SPACE_DEFAULT, 2048), 0);
  }
  // Passing non-DEFAULT strategy with threshold == 0 queries threshold only.
  ASSERT_GE(H5Pset_file_space(fcpl.get(), H5F_FILE_SPACE_ALL, 0), 0);
  ASSERT_GE(H5Pget_file_space(fcpl.get(), &strategy, &threshold), 0);
  EXPECT_EQ(strategy, H5F_FILE_SPACE_ALL);
  EXPECT_EQ(threshold, 1024u);
}

}  // namespace
}  // namespace hdf5
