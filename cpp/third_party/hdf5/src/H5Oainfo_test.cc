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
// Tests Object Header Attribute Information messages (encoding, decoding,
// sizing, copying, freeing, file deletion, cross-file copy callbacks, debug
// formatting, and compact/dense attribute lifecycles) in
// third_party/hdf5/src/H5Oainfo.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Oainfo.c

#include <stdio.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// clang-format off
// `:hdf5` is built with -DNDEBUG, omitting debug fields from cache/object
// header structs. Define NDEBUG around internal headers so struct offsets match.
#ifndef NDEBUG
#define H5OAINFO_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// H5B2private.h (pulled by H5Apkg.h) uses `try` as a parameter name, which is
// both a C++ keyword and macro-defined under -fno-exceptions.
#pragma push_macro("try")
#undef try
#define try h5b2_try_param
extern "C" {
#define H5A_FRIEND
#define H5F_FRIEND
#define H5O_FRIEND
#include "third_party/hdf5/src/H5Apkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
}  // extern "C"
#undef try
#pragma pop_macro("try")
#ifdef H5OAINFO_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OAINFO_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

// RAII wrapper for HDF5 internal API context and metadata cache tag.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__IGNORE_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (tag != H5AC__IGNORE_TAG) {
        H5AC_tag(tag, &prev_tag_);
        tagged_ = true;
      }
    }
  }

  ~ScopedApiContext() {
    if (tagged_) {
      H5AC_tag(prev_tag_, nullptr);
    }
    if (pushed_) {
      H5CX_pop(false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

// Retrieves H5F_t* and decoded H5O_ainfo_t for an open object. Returns true
// when the object uses dense attribute storage (fheap_addr defined).
bool ReadObjectAinfo(hid_t obj_id, H5F_t** out_file, H5O_ainfo_t* out_ainfo) {
  ScopedApiContext ctx;
  if (!ctx.valid()) {
    return false;
  }
  H5O_loc_t* oloc = H5O_get_loc(obj_id);
  if (oloc == nullptr || oloc->file == nullptr) {
    return false;
  }
  H5O_t* oh = H5O_protect(oloc, H5AC__READ_ONLY_FLAG, false);
  if (oh == nullptr) {
    return false;
  }
  absl::Cleanup unprotect = [&] {
    H5O_unprotect(oloc, oh, H5AC__NO_FLAGS_SET);
  };
  std::memset(out_ainfo, 0, sizeof(*out_ainfo));
  out_ainfo->fheap_addr = HADDR_UNDEF;
  out_ainfo->name_bt2_addr = HADDR_UNDEF;
  out_ainfo->corder_bt2_addr = HADDR_UNDEF;
  if (H5A__get_ainfo(oloc->file, oh, out_ainfo) < 0) {
    return false;
  }
  *out_file = oloc->file;
  return H5_addr_defined(out_ainfo->fheap_addr);
}

H5F_t* GetFilePtrFromGroup(hid_t grp_id) {
  H5O_loc_t* oloc = H5O_get_loc(grp_id);
  return oloc != nullptr ? oloc->file : nullptr;
}

TEST(H5OainfoTest, EncodeDecodeSizeCopyFreeRoundTrips) {
  ScopedFile file = CreateInMemoryFile("ainfo_roundtrip.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup root(H5Gopen2(file.get(), "/", H5P_DEFAULT));
  ASSERT_TRUE(root.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtrFromGroup(root.get());
  ASSERT_NE(f, nullptr);
  const size_t addr_sz = H5F_SIZEOF_ADDR(f);
  ASSERT_GT(addr_sz, 0u);

  for (const bool track_corder : {false, true}) {
    for (const bool index_corder : {false, true}) {
      H5O_ainfo_t src{};
      src.track_corder = track_corder;
      src.index_corder = index_corder;
      src.max_crt_idx = track_corder ? 17 : 0;
      src.corder_bt2_addr = index_corder ? haddr_t{0x3333} : HADDR_UNDEF;
      src.nattrs = 9;
      src.fheap_addr = 0x1111;
      src.name_bt2_addr = 0x2222;

      const size_t expected_sz = 2 + (track_corder ? 2 : 0) +
                                 (2 + (index_corder ? 1 : 0)) * addr_sz;
      const size_t raw_sz = H5O_msg_raw_size(f, H5O_AINFO_ID, false, &src);
      EXPECT_EQ(raw_sz, expected_sz);

      std::vector<uint8_t> encoded(raw_sz, 0xA5);
      ASSERT_GE(
          H5O_msg_encode(f, H5O_AINFO_ID, false, encoded.data(), &src), 0);

      auto* decoded = static_cast<H5O_ainfo_t*>(
          H5O_msg_decode(f, nullptr, H5O_AINFO_ID, raw_sz, encoded.data()));
      ASSERT_NE(decoded, nullptr);
      absl::Cleanup free_decoded = [&] {
        H5O_msg_free(H5O_AINFO_ID, decoded);
      };

      EXPECT_EQ(decoded->track_corder, track_corder);
      EXPECT_EQ(decoded->index_corder, index_corder);
      EXPECT_EQ(decoded->nattrs, HSIZET_MAX);
      EXPECT_EQ(decoded->max_crt_idx,
                track_corder ? src.max_crt_idx : H5O_MAX_CRT_ORDER_IDX);
      EXPECT_EQ(decoded->fheap_addr, src.fheap_addr);
      EXPECT_EQ(decoded->name_bt2_addr, src.name_bt2_addr);
      EXPECT_EQ(decoded->corder_bt2_addr,
                index_corder ? src.corder_bt2_addr : HADDR_UNDEF);

      // Copy into caller-supplied buffer.
      H5O_ainfo_t copy_inplace{};
      void* cp_ret = H5O_msg_copy(H5O_AINFO_ID, decoded, &copy_inplace);
      EXPECT_EQ(cp_ret, &copy_inplace);
      EXPECT_EQ(copy_inplace.track_corder, decoded->track_corder);
      EXPECT_EQ(copy_inplace.index_corder, decoded->index_corder);
      EXPECT_EQ(copy_inplace.fheap_addr, decoded->fheap_addr);
      EXPECT_EQ(copy_inplace.name_bt2_addr, decoded->name_bt2_addr);
      EXPECT_EQ(copy_inplace.corder_bt2_addr, decoded->corder_bt2_addr);

      // Copy with null destination allocates a new H5O_ainfo_t.
      auto* copy_alloc =
          static_cast<H5O_ainfo_t*>(H5O_msg_copy(H5O_AINFO_ID, decoded, nullptr));
      ASSERT_NE(copy_alloc, nullptr);
      absl::Cleanup free_copy = [&] {
        H5O_msg_free(H5O_AINFO_ID, copy_alloc);
      };
      EXPECT_EQ(copy_alloc->fheap_addr, decoded->fheap_addr);
      EXPECT_EQ(copy_alloc->corder_bt2_addr, decoded->corder_bt2_addr);
    }
  }
}

TEST(H5OainfoTest, DecodeRejectsInvalidAndTruncatedBuffers) {
  ScopedFile file = CreateInMemoryFile("ainfo_decode_errors.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup root(H5Gopen2(file.get(), "/", H5P_DEFAULT));
  ASSERT_TRUE(root.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtrFromGroup(root.get());
  ASSERT_NE(f, nullptr);
  const size_t addr_sz = H5F_SIZEOF_ADDR(f);

  H5O_ainfo_t template_msg{};
  template_msg.track_corder = true;
  template_msg.index_corder = true;
  template_msg.max_crt_idx = 5;
  template_msg.fheap_addr = 0x100;
  template_msg.name_bt2_addr = 0x200;
  template_msg.corder_bt2_addr = 0x300;
  const size_t full_sz =
      H5O_msg_raw_size(f, H5O_AINFO_ID, false, &template_msg);
  std::vector<uint8_t> valid_buf(full_sz, 0);
  ASSERT_GE(
      H5O_msg_encode(f, H5O_AINFO_ID, false, valid_buf.data(), &template_msg),
      0);

  ScopedErrorSilence silence;

  // Empty buffer (p_size == 0) triggers version overflow. Use index 1 of a
  // 2-byte array so (p + 0 - 1) remains within bounds.
  std::array<uint8_t, 2> dummy_backing{0, 0};
  EXPECT_EQ(
      H5O_msg_decode(f, nullptr, H5O_AINFO_ID, 0, dummy_backing.data() + 1),
      nullptr);

  // Bad version byte.
  {
    std::vector<uint8_t> bad = valid_buf;
    bad[0] = 1;
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_AINFO_ID, bad.size(), bad.data()),
              nullptr);
  }

  // Truncated before flags byte (p_size == 1).
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_AINFO_ID, 1, valid_buf.data()),
            nullptr);

  // Invalid flags bit set.
  {
    std::vector<uint8_t> bad = valid_buf;
    bad[1] = 0x04;
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_AINFO_ID, bad.size(), bad.data()),
              nullptr);
  }

  // track_corder set, truncated within max_crt_idx (2 bytes: version + flags).
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_AINFO_ID, 3, valid_buf.data()),
            nullptr);

  // Truncated inside fheap_addr (4 header bytes + 1 byte of addr).
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_AINFO_ID, 5, valid_buf.data()),
            nullptr);

  // Truncated inside name_bt2_addr (4 + addr_sz + 1 bytes).
  EXPECT_EQ(
      H5O_msg_decode(f, nullptr, H5O_AINFO_ID, 4 + addr_sz + 1, valid_buf.data()),
      nullptr);

  // index_corder set, truncated inside corder_bt2_addr (full_sz - 1 bytes).
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_AINFO_ID, full_sz - 1,
                           valid_buf.data()),
            nullptr);
}

TEST(H5OainfoTest, DebugFormattingCoversFlagCombinations) {
  ScopedFile file = CreateInMemoryFile("ainfo_debug.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup root(H5Gopen2(file.get(), "/", H5P_DEFAULT));
  ASSERT_TRUE(root.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtrFromGroup(root.get());
  ASSERT_NE(f, nullptr);

  auto format_ainfo = [&](const H5O_ainfo_t& ainfo) -> std::string {
    std::array<char, 4096> buf{};
    FILE* fp = fmemopen(buf.data(), buf.size(), "w");
    if (fp == nullptr) return "";
    herr_t rc = FAIL;
    {
      absl::Cleanup close_fp = [&] { std::fclose(fp); };
      rc = H5O_debug_id(H5O_AINFO_ID, f, &ainfo, fp, 2, 40);
    }
    if (rc < 0) {
      return "";
    }
    return std::string(buf.data());
  };

  H5O_ainfo_t tracked{};
  tracked.track_corder = true;
  tracked.index_corder = true;
  tracked.max_crt_idx = 42;
  tracked.nattrs = 7;
  tracked.fheap_addr = 1024;
  tracked.name_bt2_addr = 2048;
  tracked.corder_bt2_addr = 4096;
  const std::string tracked_str = format_ainfo(tracked);
  EXPECT_THAT(tracked_str, HasSubstr("Number of attributes:"));
  EXPECT_THAT(tracked_str, HasSubstr("TRUE"));
  EXPECT_THAT(tracked_str, HasSubstr("42"));
  EXPECT_THAT(tracked_str, HasSubstr("1024"));

  H5O_ainfo_t untracked{};
  untracked.track_corder = false;
  untracked.index_corder = false;
  untracked.max_crt_idx = 0;
  untracked.nattrs = 2;
  untracked.fheap_addr = HADDR_UNDEF;
  untracked.name_bt2_addr = HADDR_UNDEF;
  untracked.corder_bt2_addr = HADDR_UNDEF;
  const std::string untracked_str = format_ainfo(untracked);
  EXPECT_THAT(untracked_str, HasSubstr("FALSE"));
}

TEST(H5OainfoTest, DenseAndCompactLifecycleReopenCopyAndDelete) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);

  ScopedFile src_file(
      H5Fcreate("ainfo_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(src_file.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  // Target dataset for object reference attribute.
  ScopedDataset ref_target(H5Dcreate2(src_file.get(), "/ref_target",
                                      H5T_NATIVE_INT32, scalar.get(),
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ref_target.valid());
  const int32_t target_val = 77;
  ASSERT_GE(H5Dwrite(ref_target.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, &target_val),
            0);
  ref_target.reset();

  // 1. Compact attributes with creation order tracked.
  ScopedPlist dcpl_compact(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_compact.valid());
  ASSERT_GE(
      H5Pset_attr_creation_order(dcpl_compact.get(), H5P_CRT_ORDER_TRACKED), 0);
  ScopedDataset dset_compact(
      H5Dcreate2(src_file.get(), "/compact_tracked", H5T_NATIVE_INT32,
                 scalar.get(), H5P_DEFAULT, dcpl_compact.get(), H5P_DEFAULT));
  ASSERT_TRUE(dset_compact.valid());
  for (int i = 0; i < 2; ++i) {
    const std::string name = std::format("ca_{}", i);
    ScopedAttr attr(H5Acreate2(dset_compact.get(), name.c_str(), H5T_NATIVE_INT32,
                               scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, &i), 0);
  }

  // 2. Dense attributes with creation order tracked and indexed + reference.
  ScopedPlist gcpl_dense(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl_dense.valid());
  ASSERT_GE(H5Pset_attr_creation_order(
                gcpl_dense.get(),
                H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ASSERT_GE(H5Pset_attr_phase_change(gcpl_dense.get(), 2, 1), 0);
  ScopedGroup grp_dense(H5Gcreate2(src_file.get(), "/dense_indexed",
                                   H5P_DEFAULT, gcpl_dense.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp_dense.valid());
  for (int i = 0; i < 4; ++i) {
    const std::string name = std::format("da_{}", i);
    ScopedAttr attr(H5Acreate2(grp_dense.get(), name.c_str(), H5T_NATIVE_INT32,
                               scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, &i), 0);
  }
  {
    H5R_ref_t obj_ref{};
    ASSERT_GE(
        H5Rcreate_object(src_file.get(), "/ref_target", H5P_DEFAULT, &obj_ref),
        0);
    absl::Cleanup destroy_ref = [&] { H5Rdestroy(&obj_ref); };
    ScopedAttr ref_attr(H5Acreate2(grp_dense.get(), "obj_ref", H5T_STD_REF,
                                   scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ref_attr.valid());
    ASSERT_GE(H5Awrite(ref_attr.get(), H5T_STD_REF, &obj_ref), 0);
  }

  // 3. Dense attributes without creation order tracking.
  ScopedPlist gcpl_unindexed(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl_unindexed.valid());
  ASSERT_GE(H5Pset_attr_phase_change(gcpl_unindexed.get(), 0, 0), 0);
  ScopedGroup grp_unindexed(
      H5Gcreate2(src_file.get(), "/dense_unindexed", H5P_DEFAULT,
                 gcpl_unindexed.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp_unindexed.valid());
  for (int i = 0; i < 3; ++i) {
    const std::string name = std::format("ua_{}", i);
    ScopedAttr attr(H5Acreate2(grp_unindexed.get(), name.c_str(),
                               H5T_NATIVE_INT32, scalar.get(), H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, &i), 0);
  }

  ASSERT_GE(H5Fflush(src_file.get(), H5F_SCOPE_GLOBAL), 0);

  H5F_t* f_ptr = nullptr;
  H5O_ainfo_t compact_ainfo{};
  EXPECT_FALSE(ReadObjectAinfo(dset_compact.get(), &f_ptr, &compact_ainfo));
  EXPECT_TRUE(compact_ainfo.track_corder);
  H5O_ainfo_t dense_ainfo{};
  EXPECT_TRUE(ReadObjectAinfo(grp_dense.get(), &f_ptr, &dense_ainfo));
  EXPECT_TRUE(dense_ainfo.track_corder);
  EXPECT_TRUE(dense_ainfo.index_corder);
  H5O_ainfo_t unindexed_ainfo{};
  EXPECT_TRUE(ReadObjectAinfo(grp_unindexed.get(), &f_ptr, &unindexed_ainfo));
  EXPECT_FALSE(unindexed_ainfo.track_corder);

  dset_compact.reset();
  grp_dense.reset();
  grp_unindexed.reset();
  ASSERT_GE(H5Fflush(src_file.get(), H5F_SCOPE_GLOBAL), 0);

  // Extract file image and open read-only in Core VFD to force header decode.
  const ssize_t image_size = H5Fget_file_image(src_file.get(), nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  ASSERT_EQ(
      H5Fget_file_image(src_file.get(), image.data(), image.size()),
      image_size);

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(
      H5Fopen("ainfo_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());
  ScopedGroup ro_dense(H5Gopen2(ro_file.get(), "/dense_indexed", H5P_DEFAULT));
  ASSERT_TRUE(ro_dense.valid());
  H5O_info2_t oinfo{};
  ASSERT_GE(H5Oget_info3(ro_dense.get(), &oinfo, H5O_INFO_NUM_ATTRS), 0);
  EXPECT_EQ(oinfo.num_attrs, 5u);
  ro_dense.reset();

  // Copy dense/compact objects across files with default, ref-expand, and
  // without-attrs flags.
  ScopedFile dst_file(
      H5Fcreate("ainfo_dst.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(dst_file.valid());

  ASSERT_GE(H5Ocopy(src_file.get(), "/compact_tracked", dst_file.get(),
                    "/compact_copy", H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ocopy(src_file.get(), "/dense_indexed", dst_file.get(),
                    "/dense_copy", H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ocopy(src_file.get(), "/dense_unindexed", dst_file.get(),
                    "/dense_unindexed_copy", H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedPlist ocpy_ref(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpy_ref.valid());
  ASSERT_GE(
      H5Pset_copy_object(ocpy_ref.get(), H5O_COPY_EXPAND_REFERENCE_FLAG), 0);
  ASSERT_GE(H5Ocopy(src_file.get(), "/dense_indexed", dst_file.get(),
                    "/dense_ref_expanded", ocpy_ref.get(), H5P_DEFAULT),
            0);

  ScopedPlist ocpy_no_attr(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpy_no_attr.valid());
  ASSERT_GE(
      H5Pset_copy_object(ocpy_no_attr.get(), H5O_COPY_WITHOUT_ATTR_FLAG), 0);
  ASSERT_GE(H5Ocopy(src_file.get(), "/dense_indexed", dst_file.get(),
                    "/dense_no_attr", ocpy_no_attr.get(), H5P_DEFAULT),
            0);

  H5O_info2_t no_attr_info{};
  ASSERT_GE(H5Oget_info_by_name3(dst_file.get(), "/dense_no_attr",
                                 &no_attr_info, H5O_INFO_NUM_ATTRS,
                                 H5P_DEFAULT),
            0);
  EXPECT_EQ(no_attr_info.num_attrs, 0u);

  H5O_info2_t dense_copy_info{};
  ASSERT_GE(H5Oget_info_by_name3(dst_file.get(), "/dense_copy",
                                 &dense_copy_info, H5O_INFO_NUM_ATTRS,
                                 H5P_DEFAULT),
            0);
  EXPECT_EQ(dense_copy_info.num_attrs, 5u);

  // Unlinking objects triggers H5O__ainfo_delete for both compact and dense.
  ASSERT_GE(H5Ldelete(dst_file.get(), "/compact_copy", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(dst_file.get(), "/dense_copy", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(dst_file.get(), "/dense_unindexed_copy", H5P_DEFAULT),
            0);
}

TEST(H5OainfoTest, InternalDeleteCopyFileAndPostCopyFileErrors) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);

  ScopedFile rw_file(
      H5Fcreate("ainfo_err_rw.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(rw_file.valid());
  ASSERT_GE(H5Fflush(rw_file.get(), H5F_SCOPE_GLOBAL), 0);

  const ssize_t image_size = H5Fget_file_image(rw_file.get(), nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  ASSERT_EQ(H5Fget_file_image(rw_file.get(), image.data(), image.size()),
            image_size);

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(
      H5Fopen("ainfo_err_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());

  ScopedGroup rw_root(H5Gopen2(rw_file.get(), "/", H5P_DEFAULT));
  ASSERT_TRUE(rw_root.valid());
  ScopedGroup ro_root(H5Gopen2(ro_file.get(), "/", H5P_DEFAULT));
  ASSERT_TRUE(ro_root.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5O_loc_t* rw_oloc = H5O_get_loc(rw_root.get());
  H5O_loc_t* ro_oloc = H5O_get_loc(ro_root.get());
  ASSERT_NE(rw_oloc, nullptr);
  ASSERT_NE(ro_oloc, nullptr);
  ASSERT_NE(rw_oloc->file, nullptr);
  ASSERT_NE(ro_oloc->file, nullptr);

  H5O_t* oh = H5O_protect(rw_oloc, H5AC__READ_ONLY_FLAG, false);
  ASSERT_NE(oh, nullptr);
  absl::Cleanup unprotect = [&] {
    H5O_unprotect(rw_oloc, oh, H5AC__NO_FLAGS_SET);
  };

  ScopedErrorSilence silence;

  // H5O__ainfo_delete error when H5A__dense_delete fails on invalid fheap addr.
  H5O_ainfo_t bad_ainfo{};
  bad_ainfo.fheap_addr = 0xdeadbeefULL;
  bad_ainfo.name_bt2_addr = 0xfeedfaceULL;
  bad_ainfo.corder_bt2_addr = HADDR_UNDEF;
  EXPECT_LT(H5O_msg_delete(rw_oloc->file, oh, H5O_AINFO_ID, &bad_ainfo), 0);

  // H5O__ainfo_copy_file failure when H5A__dense_create fails on read-only dst.
  bool recomp = false;
  unsigned mflags = 0;
  H5O_copy_t cpy_info{};
  cpy_info.copy_without_attr = false;
  EXPECT_EQ((H5O_MSG_AINFO[0].copy_file)(rw_oloc->file, &bad_ainfo,
                                         ro_oloc->file, &recomp, &mflags,
                                         &cpy_info, nullptr),
            nullptr);

  // H5O__ainfo_post_copy_file failure when H5A__dense_post_copy_file_all fails.
  H5O_ainfo_t dst_ainfo{};
  EXPECT_LT((H5O_MSG_AINFO[0].post_copy_file)(rw_oloc, &bad_ainfo, rw_oloc,
                                              &dst_ainfo, &mflags, &cpy_info),
            0);
}

}  // namespace
}  // namespace hdf5
