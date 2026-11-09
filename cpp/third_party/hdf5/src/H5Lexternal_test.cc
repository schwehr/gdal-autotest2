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
// Tests external link class registration, traversal, property-list
// customization, callback error handling, malformed link payloads, and
// query callbacks (third_party/hdf5/src/H5Lexternal.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Lexternal.c

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG; match internal struct layout around
// `hdf5_internal_prelude.h` and internal private headers.
#ifndef NDEBUG
#define H5LEXTERNAL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Lprivate.h"
}  // extern "C"
#ifdef H5LEXTERNAL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5LEXTERNAL_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  ScopedApiContext() { pushed_ = (H5CX_push(&node_) >= 0); }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

struct ElinkCbRecord {
  int calls = 0;
  std::string parent_file;
  std::string parent_group;
  std::string child_file;
  std::string child_object;
  unsigned seen_flags = 0;
  unsigned override_flags = H5F_ACC_DEFAULT;
  bool set_override_flags = false;
  herr_t return_code = 0;
};

herr_t RecordElinkCallback(const char* parent_file_name,
                           const char* parent_group_name,
                           const char* child_file_name,
                           const char* child_object_name, unsigned* acc_flags,
                           hid_t fapl_id, void* op_data) {
  auto* rec = static_cast<ElinkCbRecord*>(op_data);
  if (rec == nullptr || fapl_id < 0 || acc_flags == nullptr) {
    return -1;
  }
  ++rec->calls;
  rec->parent_file = parent_file_name ? parent_file_name : "";
  rec->parent_group = parent_group_name ? parent_group_name : "";
  rec->child_file = child_file_name ? child_file_name : "";
  rec->child_object = child_object_name ? child_object_name : "";
  rec->seen_flags = *acc_flags;
  if (rec->set_override_flags) {
    *acc_flags = rec->override_flags;
  }
  return rec->return_code;
}

herr_t RemoveCloseDegreeCallback(const char* /*parent_file_name*/,
                                 const char* /*parent_group_name*/,
                                 const char* /*child_file_name*/,
                                 const char* /*child_object_name*/,
                                 unsigned* /*acc_flags*/, hid_t fapl_id,
                                 void* op_data) {
  auto* removed = static_cast<bool*>(op_data);
  if (removed != nullptr) {
    *removed = (H5Premove(fapl_id, H5F_ACS_CLOSE_DEGREE_NAME) >= 0);
  }
  return 0;
}

hid_t DummyExternalTraverse(const char* /*link_name*/, hid_t /*cur_group*/,
                            const void* /*udata*/, size_t /*udata_size*/,
                            hid_t /*lapl_id*/, hid_t /*dxpl_id*/) {
  return H5I_INVALID_HID;
}

void CreateTargetFile(const std::filesystem::path& target_path) {
  std::error_code ec;
  std::filesystem::create_directories(target_path.parent_path(), ec);
  ScopedFile target(
      H5Fcreate(target_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(target.valid()) << target_path;

  ScopedGroup grp(
      H5Gcreate2(target.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedDataset dset(H5Dcreate2(grp.get(), "dset", H5T_NATIVE_INT32,
                                scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  constexpr int32_t kValue = 4242;
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, &kValue),
            0);
}

TEST(H5LexternalTest, RegisterExternalRestoresDefaultClass) {
  ASSERT_GE(H5open(), 0);
  // Direct call ensures H5L_register_external is linked and exercised.
  ASSERT_GE(H5L_register_external(), 0);
  EXPECT_GT(H5Lis_registered(H5L_TYPE_EXTERNAL), 0);

  absl::Cleanup restore_external = [] { (void)H5L_register_external(); };

  ASSERT_GE(H5Lunregister(H5L_TYPE_EXTERNAL), 0);
  EXPECT_EQ(H5Lis_registered(H5L_TYPE_EXTERNAL), 0);

  H5L_class_t dummy_cls{};
  dummy_cls.version = H5L_LINK_CLASS_T_VERS;
  dummy_cls.id = H5L_TYPE_EXTERNAL;
  dummy_cls.comment = "temporary_override";
  dummy_cls.trav_func = DummyExternalTraverse;
  ASSERT_GE(H5Lregister(&dummy_cls), 0);
  EXPECT_GT(H5Lis_registered(H5L_TYPE_EXTERNAL), 0);

  // H5L_register_external restores the library's default external link class.
  ASSERT_GE(H5L_register_external(), 0);
  EXPECT_GT(H5Lis_registered(H5L_TYPE_EXTERNAL), 0);
}

TEST(H5LexternalTest, TraverseWithDefaultAndCustomLaplProperties) {
  TempDir dir("h5lext_traverse");
  const std::filesystem::path sub_dir = dir.child("sub");
  const std::filesystem::path target_path = sub_dir / "target.h5";
  const std::filesystem::path parent_path = dir.child("parent.h5");

  CreateTargetFile(target_path);

  const std::string long_group_name(280, 'p');
  const std::string long_group_path = "/" + long_group_name;
  const std::string long_link_path = long_group_path + "/to_target";

  {
    ScopedFile parent(H5Fcreate(parent_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                                H5P_DEFAULT));
    ASSERT_TRUE(parent.valid());

    ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
    ASSERT_TRUE(lcpl.valid());
    ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1), 0);

    ASSERT_GE(H5Lcreate_external(target_path.c_str(), "/grp/dset", parent.get(),
                                 "to_abs", H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_external("target.h5", "/grp/dset", parent.get(),
                                 "to_prefixed", H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(
        H5Lcreate_external(target_path.c_str(), "/grp", parent.get(),
                           long_link_path.c_str(), lcpl.get(), H5P_DEFAULT),
        0);
  }

  ScopedFile parent(H5Fopen(parent_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
  ASSERT_TRUE(parent.valid());

  // 1. Default LAPL: inherits H5F_ACC_RDWR intent and parent FAPL.
  {
    ScopedObject dset(H5Oopen(parent.get(), "to_abs", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    int32_t read_val = 0;
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, &read_val),
              0);
    EXPECT_EQ(read_val, 4242);
  }

  // 2. Custom LAPL with elink prefix, explicit FAPL, RDONLY flags, and
  // callback.
  {
    ScopedPlist custom_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(custom_fapl.valid());

    ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
    ASSERT_TRUE(lapl.valid());
    ASSERT_GE(H5Pset_elink_prefix(lapl.get(), sub_dir.c_str()), 0);
    ASSERT_GE(H5Pset_elink_fapl(lapl.get(), custom_fapl.get()), 0);
    ASSERT_GE(H5Pset_elink_acc_flags(lapl.get(), H5F_ACC_RDONLY), 0);

    ElinkCbRecord cb_rec;
    ASSERT_GE(H5Pset_elink_cb(lapl.get(), RecordElinkCallback, &cb_rec), 0);

    ScopedObject dset(H5Oopen(parent.get(), "to_prefixed", lapl.get()));
    ASSERT_TRUE(dset.valid());
    EXPECT_EQ(cb_rec.calls, 1);
    EXPECT_EQ(cb_rec.parent_group, "/");
    EXPECT_EQ(cb_rec.child_file, "target.h5");
    EXPECT_EQ(cb_rec.child_object, "/grp/dset");
    EXPECT_EQ(cb_rec.seen_flags, H5F_ACC_RDONLY);

    // 3. Long parent group name (> 256 bytes) triggers heap allocation in
    // H5L__extern_traverse when formatting parent_group_name for cb_info.func.
    ElinkCbRecord long_cb_rec;
    ASSERT_GE(H5Pset_elink_cb(lapl.get(), RecordElinkCallback, &long_cb_rec),
              0);
    ScopedObject grp(H5Oopen(parent.get(), long_link_path.c_str(), lapl.get()));
    ASSERT_TRUE(grp.valid());
    EXPECT_EQ(long_cb_rec.calls, 1);
    EXPECT_EQ(long_cb_rec.parent_group, long_group_path);
    EXPECT_EQ(long_cb_rec.child_object, "/grp");
  }
}

TEST(H5LexternalTest, TraverseAndQueryRejectMalformedPayloadsAndFailures) {
  TempDir dir("h5lext_errors");
  const std::filesystem::path target_path = dir.child("target.h5");
  const std::filesystem::path missing_file = dir.child("does_not_exist.h5");
  const std::filesystem::path parent_path = dir.child("parent_err.h5");

  CreateTargetFile(target_path);

  ScopedFile parent(
      H5Fcreate(parent_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(parent.valid());

  // Valid target file, missing object inside it.
  ASSERT_GE(
      H5Lcreate_external(target_path.c_str(), "/no_such_obj", parent.get(),
                         "bad_obj", H5P_DEFAULT, H5P_DEFAULT),
      0);
  // Missing target file.
  ASSERT_GE(H5Lcreate_external(missing_file.c_str(), "/grp", parent.get(),
                               "bad_file", H5P_DEFAULT, H5P_DEFAULT),
            0);
  // Valid external link for testing callback failures.
  ASSERT_GE(H5Lcreate_external(target_path.c_str(), "/grp", parent.get(),
                               "good_ext", H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Create malformed H5L_TYPE_EXTERNAL udata via H5Lcreate_ud (H5Lexternal's
  // create_func is NULL so raw payload bytes are stored verbatim).
  constexpr std::array<uint8_t, 5> kBadVerUdata = {0x10, 'f', '\0', 'o', '\0'};
  constexpr std::array<uint8_t, 5> kBadFlagUdata = {0x01, 'f', '\0', 'o', '\0'};
  ASSERT_GE(H5Lcreate_ud(parent.get(), "bad_ver", H5L_TYPE_EXTERNAL,
                         kBadVerUdata.data(), kBadVerUdata.size(), H5P_DEFAULT,
                         H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_ud(parent.get(), "bad_flags", H5L_TYPE_EXTERNAL,
                         kBadFlagUdata.data(), kBadFlagUdata.size(),
                         H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedErrorSilence silence;

  // Traverse errors: missing file, missing object, bad version, bad flags.
  EXPECT_EQ(H5Oopen(parent.get(), "bad_file", H5P_DEFAULT), H5I_INVALID_HID);
  EXPECT_EQ(H5Oopen(parent.get(), "bad_obj", H5P_DEFAULT), H5I_INVALID_HID);
  EXPECT_EQ(H5Oopen(parent.get(), "bad_ver", H5P_DEFAULT), H5I_INVALID_HID);
  EXPECT_EQ(H5Oopen(parent.get(), "bad_flags", H5P_DEFAULT), H5I_INVALID_HID);

  // Query errors in H5L__extern_query: bad version and bad flags.
  H5L_info2_t linfo{};
  std::array<uint8_t, 16> val_buf{};
  EXPECT_LT(H5Lget_info2(parent.get(), "bad_ver", &linfo, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_info2(parent.get(), "bad_flags", &linfo, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_val(parent.get(), "bad_ver", val_buf.data(), val_buf.size(),
                       H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_val(parent.get(), "bad_flags", val_buf.data(),
                       val_buf.size(), H5P_DEFAULT),
            0);

  // Callback failure and invalid modified access flags (TRUNC / EXCL).
  ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl.valid());

  ElinkCbRecord cb_fail;
  cb_fail.return_code = -1;
  ASSERT_GE(H5Pset_elink_cb(lapl.get(), RecordElinkCallback, &cb_fail), 0);
  EXPECT_EQ(H5Oopen(parent.get(), "good_ext", lapl.get()), H5I_INVALID_HID);

  ElinkCbRecord cb_trunc;
  cb_trunc.set_override_flags = true;
  cb_trunc.override_flags = H5F_ACC_RDWR | H5F_ACC_TRUNC;
  ASSERT_GE(H5Pset_elink_cb(lapl.get(), RecordElinkCallback, &cb_trunc), 0);
  EXPECT_EQ(H5Oopen(parent.get(), "good_ext", lapl.get()), H5I_INVALID_HID);

  ElinkCbRecord cb_excl;
  cb_excl.set_override_flags = true;
  cb_excl.override_flags = H5F_ACC_RDWR | H5F_ACC_EXCL;
  ASSERT_GE(H5Pset_elink_cb(lapl.get(), RecordElinkCallback, &cb_excl), 0);
  EXPECT_EQ(H5Oopen(parent.get(), "good_ext", lapl.get()), H5I_INVALID_HID);
}

TEST(H5LexternalTest, QueryReturnsValSizeAndTruncatedOrFullUdata) {
  ScopedFile file = CreateInMemoryFile("h5lext_query.h5");
  ASSERT_TRUE(file.valid());

  ASSERT_GE(H5Lcreate_external("ext_target.h5", "/sub/obj", file.get(), "ext",
                               H5P_DEFAULT, H5P_DEFAULT),
            0);

  // NULL buffer query via H5Lget_info2 populates val_size.
  H5L_info2_t info{};
  ASSERT_GE(H5Lget_info2(file.get(), "ext", &info, H5P_DEFAULT), 0);
  EXPECT_EQ(info.type, H5L_TYPE_EXTERNAL);
  // 1 version/flag byte + "ext_target.h5\0" (14) + "/sub/obj\0" (9) = 24.
  EXPECT_EQ(info.u.val_size, 24u);

  // Full buffer query (buf_size > udata_size branch).
  std::vector<uint8_t> full_buf(info.u.val_size + 8, 0xAA);
  ASSERT_GE(H5Lget_val(file.get(), "ext", full_buf.data(), full_buf.size(),
                       H5P_DEFAULT),
            0);
  unsigned flags = 99;
  const char* fname = nullptr;
  const char* obj_path = nullptr;
  ASSERT_GE(H5Lunpack_elink_val(full_buf.data(), info.u.val_size, &flags,
                                &fname, &obj_path),
            0);
  EXPECT_EQ(flags, 0u);
  EXPECT_STREQ(fname, "ext_target.h5");
  EXPECT_STREQ(obj_path, "/sub/obj");

  // Truncated buffer query (buf_size < udata_size branch).
  std::array<uint8_t, 5> small_buf{};
  small_buf.fill(0x55);
  ASSERT_GE(H5Lget_val(file.get(), "ext", small_buf.data(), small_buf.size(),
                       H5P_DEFAULT),
            0);
  EXPECT_EQ(small_buf[0], 0u);
  EXPECT_EQ(small_buf[1], 'e');
  EXPECT_EQ(small_buf[4], '_');
}

TEST(H5LexternalTest, DirectTraverseClassValidatesLaplAndGroupIds) {
  ScopedFile file = CreateInMemoryFile("h5lext_direct.h5");
  ASSERT_TRUE(file.valid());

  const H5L_class_t* cls = H5L_find_class(H5L_TYPE_EXTERNAL);
  ASSERT_NE(cls, nullptr);
  ASSERT_NE(cls->trav_func, nullptr);

  constexpr std::array<uint8_t, 6> kValidUdata = {0x00, 'f',  '\0',
                                                  '/',  '\0', '\0'};

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  // Invalid lapl_id hits H5P_object_verify failure inside H5L__extern_traverse.
  EXPECT_EQ(
      cls->trav_func("ext", file.get(), kValidUdata.data(), kValidUdata.size(),
                     H5I_INVALID_HID, H5P_DATASET_XFER_DEFAULT),
      H5I_INVALID_HID);

  // Invalid cur_group with H5P_DEFAULT fails H5P_object_verify first.
  EXPECT_EQ(
      cls->trav_func("ext", H5I_INVALID_HID, kValidUdata.data(),
                     kValidUdata.size(), H5P_DEFAULT, H5P_DATASET_XFER_DEFAULT),
      H5I_INVALID_HID);

  // Valid lapl_id with invalid cur_group hits H5G_loc failure inside
  // H5L__extern_traverse.
  ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl.valid());
  EXPECT_EQ(
      cls->trav_func("ext", H5I_INVALID_HID, kValidUdata.data(),
                     kValidUdata.size(), lapl.get(), H5P_DATASET_XFER_DEFAULT),
      H5I_INVALID_HID);
}

TEST(H5LexternalTest, TraverseFailsWhenRequiredPlistPropertiesRemoved) {
  TempDir dir("h5lext_missing_props");
  const std::filesystem::path target_path = dir.child("target.h5");
  const std::filesystem::path parent_path = dir.child("parent.h5");
  CreateTargetFile(target_path);

  {
    ScopedFile created_parent(H5Fcreate(parent_path.c_str(), H5F_ACC_TRUNC,
                                        H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(created_parent.valid());
    ASSERT_GE(H5Lcreate_external(target_path.c_str(), "/grp/dset",
                                 created_parent.get(), "ext", H5P_DEFAULT,
                                 H5P_DEFAULT),
              0);
  }

  // Reopen with H5F_ACC_RDWR so inherited intent does not carry H5F_ACC_TRUNC.
  ScopedFile parent(H5Fopen(parent_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
  ASSERT_TRUE(parent.valid());

  ScopedErrorSilence silence;

  constexpr std::array<const char*, 4> kLaplPropNames = {
      H5L_ACS_ELINK_FAPL_NAME,
      H5L_ACS_ELINK_FLAGS_NAME,
      H5L_ACS_ELINK_CB_NAME,
      H5L_ACS_ELINK_PREFIX_NAME,
  };
  for (const char* prop_name : kLaplPropNames) {
    ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
    ASSERT_TRUE(lapl.valid());
    ASSERT_GE(H5Premove(lapl.get(), prop_name), 0) << prop_name;
    EXPECT_EQ(H5Oopen(parent.get(), "ext", lapl.get()), H5I_INVALID_HID)
        << prop_name;
  }

  // Removing H5F_ACS_CLOSE_DEGREE_NAME from fapl_id during the callback causes
  // H5L__extern_traverse's subsequent H5P_set on fa_plist to fail cleanly.
  ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl.valid());
  bool removed_close_degree = false;
  ASSERT_GE(H5Pset_elink_cb(lapl.get(), RemoveCloseDegreeCallback,
                            &removed_close_degree),
            0);
  EXPECT_EQ(H5Oopen(parent.get(), "ext", lapl.get()), H5I_INVALID_HID);
  EXPECT_TRUE(removed_close_degree);
}

}  // namespace
}  // namespace hdf5
