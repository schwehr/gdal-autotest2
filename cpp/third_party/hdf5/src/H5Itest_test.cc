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
// Tests H5I__get_name_test internal name lookup and VOL wrapper error paths
// (third_party/hdf5/src/H5Itest.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Itest.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5I_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5I_FRIEND
#define H5I_TESTING
#include "third_party/hdf5/src/H5Ipkg.h"
#include "third_party/hdf5/src/H5VLnative_private.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5I_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5I_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

bool g_fail_get_wrap_ctx = false;
bool g_reset_wrap_ctx_once = false;

herr_t MaybeFailGetWrapCtx(const void* /*obj*/, void** wrap_ctx) {
  if (g_fail_get_wrap_ctx) {
    return -1;
  }
  *wrap_ctx = nullptr;
  return 0;
}

herr_t SucceedNullGetWrapCtx(const void* /*obj*/, void** wrap_ctx) {
  *wrap_ctx = nullptr;
  return 0;
}

void* ReturnNullGetObject(const void* /*obj*/) { return nullptr; }

// Releases the active VOL wrap context on the first lookup so H5I__get_name_test
// exercises the H5VL_reset_vol_wrapper() < 0 cleanup branch without leaking
// an H5VL_wrap_ctx_t or leaving dangling connector reference counts.
void* ResetWrapCtxOnceAndReturnNull(const void* /*obj*/) {
  if (g_reset_wrap_ctx_once) {
    g_reset_wrap_ctx_once = false;
    (void)H5VL_reset_vol_wrapper();
  }
  return nullptr;
}

herr_t SucceedFreeWrapCtx(void* /*wrap_ctx*/) { return 0; }

herr_t NoOpCloseGroup(void* /*grp*/, hid_t /*dxpl_id*/, void** /*req*/) {
  return 0;
}

TEST(H5ItestTest, CachedAndUncachedNamesAndTruncation) {
  ScopedFile file = CreateInMemoryFile("h5itest_names.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup group(H5Gcreate2(file.get(), "/alpha", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
  ASSERT_TRUE(group.valid());

  // Cached lookup via opened user path.
  bool cached = false;
  EXPECT_EQ(H5I__get_name_test(group.get(), nullptr, 0, &cached), 6);
  EXPECT_TRUE(cached);

  std::array<char, 16> buf{};
  cached = false;
  EXPECT_EQ(H5I__get_name_test(group.get(), buf.data(), buf.size(), &cached),
            6);
  EXPECT_TRUE(cached);
  EXPECT_STREQ(buf.data(), "/alpha");

  // Truncated buffer when cached path is present.
  std::array<char, 4> short_buf{};
  cached = false;
  EXPECT_EQ(
      H5I__get_name_test(group.get(), short_buf.data(), short_buf.size(),
                         &cached),
      6);
  EXPECT_TRUE(cached);
  EXPECT_STREQ(short_buf.data(), "/al");

  // Invalidate cached user path using an alternate hard link while unlinking
  // the original link name so name must be discovered by object address.
  ASSERT_GE(H5Lcreate_hard(file.get(), "/alpha", file.get(), "/beta_link",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ldelete(file.get(), "/alpha", H5P_DEFAULT), 0);

  std::array<char, 32> uncached_buf{};
  cached = true;
  EXPECT_EQ(H5I__get_name_test(group.get(), uncached_buf.data(),
                               uncached_buf.size(), &cached),
            10);
  EXPECT_FALSE(cached);
  EXPECT_STREQ(uncached_buf.data(), "/beta_link");

  // Root group via file ID has cached user path "/".
  std::array<char, 8> root_buf{};
  cached = false;
  EXPECT_EQ(
      H5I__get_name_test(file.get(), root_buf.data(), root_buf.size(), &cached),
      1);
  EXPECT_TRUE(cached);
  EXPECT_STREQ(root_buf.data(), "/");
}

TEST(H5ItestTest, InvalidIdsAndVolWrapperAndLocFailurePaths) {
  // Ensure H5I_GROUP class is registered before registering dummy VOL objects.
  ScopedFile file = CreateInMemoryFile("h5itest_vol.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup real_group(H5Gcreate2(file.get(), "/init", H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(real_group.valid());

  ScopedErrorSilence silence;
  bool cached = false;
  EXPECT_LT(H5I__get_name_test(H5I_INVALID_HID, nullptr, 0, &cached), 0);

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  EXPECT_LT(H5I__get_name_test(space.get(), nullptr, 0, &cached), 0);

  ScopedType transient_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(transient_dt.valid());
  EXPECT_LT(H5I__get_name_test(transient_dt.get(), nullptr, 0, &cached), 0);

  // Connector whose get_wrap_ctx fails during H5I__get_name_test (toggled off
  // before ScopedGroup closes so HDF5 terminates cleanly without leaks).
  H5VL_class_t fail_wrap_cls{};
  fail_wrap_cls.version = H5VL_VERSION;
  fail_wrap_cls.value = static_cast<H5VL_class_value_t>(H5_VOL_RESERVED + 11);
  fail_wrap_cls.name = "h5i_fail_wrap_ctx_vol";
  fail_wrap_cls.wrap_cls.get_wrap_ctx = MaybeFailGetWrapCtx;
  fail_wrap_cls.wrap_cls.free_wrap_ctx = SucceedFreeWrapCtx;
  fail_wrap_cls.group_cls.close = NoOpCloseGroup;

  const hid_t fail_wrap_conn_id =
      H5VLregister_connector(&fail_wrap_cls, H5P_DEFAULT);
  ASSERT_GE(fail_wrap_conn_id, 0);
  absl::Cleanup unregister_fail_wrap = [fail_wrap_conn_id] {
    g_fail_get_wrap_ctx = false;
    H5VLunregister_connector(fail_wrap_conn_id);
  };
  auto* fail_wrap_conn = static_cast<H5VL_connector_t*>(
      H5I_object_verify(fail_wrap_conn_id, H5I_VOL));
  ASSERT_NE(fail_wrap_conn, nullptr);

  int dummy_group_data = 1;
  ScopedGroup fail_wrap_grp(
      H5VL_register(H5I_GROUP, &dummy_group_data, fail_wrap_conn, true));
  ASSERT_TRUE(fail_wrap_grp.valid());
  g_fail_get_wrap_ctx = true;
  EXPECT_LT(H5I__get_name_test(fail_wrap_grp.get(), nullptr, 0, &cached), 0);
  g_fail_get_wrap_ctx = false;

  // Connector whose get_object returns nullptr -> H5G_loc error branch.
  H5VL_class_t null_obj_cls{};
  null_obj_cls.version = H5VL_VERSION;
  null_obj_cls.value = static_cast<H5VL_class_value_t>(H5_VOL_RESERVED + 12);
  null_obj_cls.name = "h5i_null_get_object_vol";
  null_obj_cls.wrap_cls.get_object = ReturnNullGetObject;
  null_obj_cls.wrap_cls.get_wrap_ctx = SucceedNullGetWrapCtx;
  null_obj_cls.wrap_cls.free_wrap_ctx = SucceedFreeWrapCtx;
  null_obj_cls.group_cls.close = NoOpCloseGroup;

  const hid_t null_obj_conn_id =
      H5VLregister_connector(&null_obj_cls, H5P_DEFAULT);
  ASSERT_GE(null_obj_conn_id, 0);
  absl::Cleanup unregister_null_obj = [null_obj_conn_id] {
    H5VLunregister_connector(null_obj_conn_id);
  };
  auto* null_obj_conn = static_cast<H5VL_connector_t*>(
      H5I_object_verify(null_obj_conn_id, H5I_VOL));
  ASSERT_NE(null_obj_conn, nullptr);

  ScopedGroup null_obj_grp(
      H5VL_register(H5I_GROUP, &dummy_group_data, null_obj_conn, true));
  ASSERT_TRUE(null_obj_grp.valid());
  EXPECT_LT(H5I__get_name_test(null_obj_grp.get(), nullptr, 0, &cached), 0);

  // Connector whose get_object clears the wrap context cleanly before returning
  // nullptr -> exercises H5VL_reset_vol_wrapper() < 0 in cleanup without leak.
  H5VL_class_t reset_ctx_cls{};
  reset_ctx_cls.version = H5VL_VERSION;
  reset_ctx_cls.value = static_cast<H5VL_class_value_t>(H5_VOL_RESERVED + 13);
  reset_ctx_cls.name = "h5i_reset_wrap_ctx_vol";
  reset_ctx_cls.wrap_cls.get_object = ResetWrapCtxOnceAndReturnNull;
  reset_ctx_cls.wrap_cls.get_wrap_ctx = SucceedNullGetWrapCtx;
  reset_ctx_cls.wrap_cls.free_wrap_ctx = SucceedFreeWrapCtx;
  reset_ctx_cls.group_cls.close = NoOpCloseGroup;

  const hid_t reset_ctx_conn_id =
      H5VLregister_connector(&reset_ctx_cls, H5P_DEFAULT);
  ASSERT_GE(reset_ctx_conn_id, 0);
  absl::Cleanup unregister_reset_ctx = [reset_ctx_conn_id] {
    g_reset_wrap_ctx_once = false;
    H5VLunregister_connector(reset_ctx_conn_id);
  };
  auto* reset_ctx_conn = static_cast<H5VL_connector_t*>(
      H5I_object_verify(reset_ctx_conn_id, H5I_VOL));
  ASSERT_NE(reset_ctx_conn, nullptr);

  ScopedGroup reset_ctx_grp(
      H5VL_register(H5I_GROUP, &dummy_group_data, reset_ctx_conn, true));
  ASSERT_TRUE(reset_ctx_grp.valid());
  g_reset_wrap_ctx_once = true;
  EXPECT_LT(H5I__get_name_test(reset_ctx_grp.get(), nullptr, 0, &cached), 0);
  g_reset_wrap_ctx_once = false;
}

TEST(H5ItestTest, GetNameFailsWhenDirectoryTraversalEncountersCorruptGroup) {
  // Build a file where /broken is the only root link and /broken/target sits
  // underneath it. Corrupting /broken's object header in the serialized image
  // causes H5G_get_name -> H5G_visit to fail while H5Oopen_by_token still opens
  // /broken/target with user_path_r == nullptr.
  haddr_t broken_addr = HADDR_UNDEF;
  H5O_token_t target_token{};
  std::vector<uint8_t> image;

  {
    ScopedFile src = CreateInMemoryFile("h5itest_corrupt_src.h5");
    ASSERT_TRUE(src.valid());
    ScopedGroup broken(H5Gcreate2(src.get(), "/broken", H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(broken.valid());
    ScopedGroup target(H5Gcreate2(broken.get(), "target", H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(target.valid());
    ASSERT_GE(H5Fflush(src.get(), H5F_SCOPE_GLOBAL), 0);

    H5O_info2_t broken_info{};
    ASSERT_GE(H5Oget_info3(broken.get(), &broken_info, H5O_INFO_BASIC), 0);
    void* file_vol_data = H5VL_object(src.get());
    ASSERT_NE(file_vol_data, nullptr);
    ASSERT_GE(H5VL_native_token_to_addr(file_vol_data, H5I_FILE,
                                        broken_info.token, &broken_addr),
              0);

    H5O_info2_t target_info{};
    ASSERT_GE(H5Oget_info3(target.get(), &target_info, H5O_INFO_BASIC), 0);
    target_token = target_info.token;

    target.reset();
    broken.reset();

    const ssize_t image_size = H5Fget_file_image(src.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(src.get(), image.data(), image.size()),
              image_size);
  }

  ASSERT_NE(broken_addr, HADDR_UNDEF);
  ASSERT_LT(broken_addr + 8, image.size());
  for (size_t i = 0; i < 8; ++i) {
    image[broken_addr + i] ^= 0xffU;
  }

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile corrupt_file(
      H5Fopen("h5itest_corrupt.h5", H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(corrupt_file.valid());

  ScopedGroup uncached_target(H5Oopen_by_token(corrupt_file.get(), target_token));
  ASSERT_TRUE(uncached_target.valid());

  ScopedErrorSilence silence;
  bool cached = true;
  EXPECT_LT(
      H5I__get_name_test(uncached_target.get(), nullptr, 0, &cached), 0);
}

}  // namespace
}  // namespace hdf5
