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
// Tests Object Header Symbol Table messages (encoding, decoding, copying,
// sizing, freeing, deletion, cross-file copying, post-copy traversal, and
// debug formatting) in third_party/hdf5/src/H5Ostab.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ostab.c

#include <stdio.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// clang-format off
// `:hdf5` is compiled with -DNDEBUG, omitting debug fields from cache/object
// header structs. Define NDEBUG around internal headers so struct offsets match.
#ifndef NDEBUG
#define H5OSTAB_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#define try hdf5_try_param
extern "C" {
#define H5F_FRIEND
#define H5G_FRIEND
#define H5O_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Gpkg.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5OSTAB_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OSTAB_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

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
      H5CX_pop(/*update_dxpl_props=*/false);
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

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

ScopedPlist MakeEarliestCoreFapl() {
  ScopedPlist fapl = MakeCoreFapl();
  if (fapl.valid()) {
    if (H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                             H5F_LIBVER_LATEST) < 0) {
      fapl.reset();
    }
  }
  return fapl;
}

TEST(H5OstabTest, EncodeDecodeSizeCopyFreeAcrossAddrSizes) {
  for (const size_t sizeof_addr : {size_t{4}, size_t{8}}) {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_sizes(fcpl.get(), sizeof_addr, sizeof_addr), 0);

    ScopedFile file = CreateInMemoryFile("stab_roundtrip.h5", H5F_ACC_TRUNC,
                                           fcpl.get());
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    H5O_stab_t src{};
    src.btree_addr = sizeof_addr == 4 ? haddr_t{0x10203040ULL}
                                      : haddr_t{0x1122334455667788ULL};
    src.heap_addr = sizeof_addr == 4 ? haddr_t{0x50607080ULL}
                                     : haddr_t{0x8877665544332211ULL};

    const size_t raw_sz =
        H5O_msg_raw_size(f, H5O_STAB_ID, /*disable_shared=*/false, &src);
    EXPECT_EQ(raw_sz, 2 * sizeof_addr);

    std::vector<uint8_t> encoded(raw_sz, 0xEEu);
    ASSERT_GE(H5O_msg_encode(f, H5O_STAB_ID, /*disable_shared=*/false,
                             encoded.data(), &src),
              0);

    auto* decoded = static_cast<H5O_stab_t*>(H5O_msg_decode(
        f, nullptr, H5O_STAB_ID, encoded.size(), encoded.data()));
    ASSERT_NE(decoded, nullptr);
    absl::Cleanup free_decoded = [&] { H5O_msg_free(H5O_STAB_ID, decoded); };

    EXPECT_EQ(decoded->btree_addr, src.btree_addr);
    EXPECT_EQ(decoded->heap_addr, src.heap_addr);

    // Copy in-place.
    H5O_stab_t copy_inplace{};
    void* copy_ret = H5O_msg_copy(H5O_STAB_ID, decoded, &copy_inplace);
    EXPECT_EQ(copy_ret, &copy_inplace);
    EXPECT_EQ(copy_inplace.btree_addr, src.btree_addr);
    EXPECT_EQ(copy_inplace.heap_addr, src.heap_addr);

    // Copy with allocation.
    auto* copy_alloc =
        static_cast<H5O_stab_t*>(H5O_msg_copy(H5O_STAB_ID, decoded, nullptr));
    ASSERT_NE(copy_alloc, nullptr);
    absl::Cleanup free_copy = [&] { H5O_msg_free(H5O_STAB_ID, copy_alloc); };
    EXPECT_EQ(copy_alloc->btree_addr, src.btree_addr);
    EXPECT_EQ(copy_alloc->heap_addr, src.heap_addr);
  }
}

TEST(H5OstabTest, DecodeRejectsTruncatedBuffers) {
  ScopedFile file = CreateInMemoryFile("stab_decode_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  const size_t addr_sz = H5F_SIZEOF_ADDR(f);
  ASSERT_GT(addr_sz, 1u);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5O_stab_t msg{};
  msg.btree_addr = 0x1000;
  msg.heap_addr = 0x2000;
  std::vector<uint8_t> valid_buf(2 * addr_sz, 0);
  ASSERT_GE(H5O_msg_encode(f, H5O_STAB_ID, /*disable_shared=*/false,
                           valid_buf.data(), &msg),
            0);

  ScopedErrorSilence silence;

  // Empty buffer (p_size == 0) fails btree_addr decode.
  std::array<uint8_t, 2> dummy{0u, 0u};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_STAB_ID, 0, dummy.data() + 1),
            nullptr);

  // Truncated in btree_addr.
  EXPECT_EQ(
      H5O_msg_decode(f, nullptr, H5O_STAB_ID, addr_sz - 1, valid_buf.data()),
      nullptr);

  // Truncated in heap_addr (at addr_sz and 2*addr_sz - 1).
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_STAB_ID, addr_sz, valid_buf.data()),
            nullptr);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_STAB_ID, 2 * addr_sz - 1,
                           valid_buf.data()),
            nullptr);
}

TEST(H5OstabTest, CopyFileDeleteAndDebugInV1FormatGroup) {
  ScopedPlist fapl = MakeEarliestCoreFapl();
  ASSERT_TRUE(fapl.valid());

  ScopedFile src_file(
      H5Fcreate("stab_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(src_file.valid());

  // Create old-format group containing nested subgroup and dataset so B-tree
  // and local heap are populated.
  ScopedGroup src_grp(
      H5Gcreate2(src_file.get(), "/v1_grp", H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT));
  ASSERT_TRUE(src_grp.valid());
  ScopedGroup child_grp(
      H5Gcreate2(src_grp.get(), "sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(child_grp.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(child_grp.get(), "leaf", H5T_NATIVE_INT,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  const int val = 99;
  ASSERT_GE(
      H5Dwrite(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val),
      0);
  dset.reset();
  child_grp.reset();

  // Verify H5O_STAB_ID exists on /v1_grp and dump via H5O_debug & H5O_debug_id.
  H5O_stab_t orig_stab{};
  H5O_loc_t* src_oloc = H5O_get_loc(src_grp.get());
  ASSERT_NE(src_oloc, nullptr);
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    EXPECT_EQ(H5O_msg_exists(src_oloc, H5O_STAB_ID), 1);
    ASSERT_NE(H5O_msg_read(src_oloc, H5O_STAB_ID, &orig_stab), nullptr);

    char* dbg_buf = nullptr;
    size_t dbg_len = 0;
    FILE* stream = open_memstream(&dbg_buf, &dbg_len);
    ASSERT_NE(stream, nullptr);
    absl::Cleanup free_dbg = [&] { std::free(dbg_buf); };
    EXPECT_GE(H5O_debug(src_oloc->file, src_oloc->addr, stream, 0, 24), 0);
    std::fclose(stream);
    const std::string dbg(dbg_buf != nullptr ? dbg_buf : "", dbg_len);
    EXPECT_THAT(dbg, HasSubstr("B-tree address:"));
    EXPECT_THAT(dbg, HasSubstr("Name heap address:"));

    char* id_buf = nullptr;
    size_t id_len = 0;
    FILE* id_stream = open_memstream(&id_buf, &id_len);
    ASSERT_NE(id_stream, nullptr);
    absl::Cleanup free_id = [&] { std::free(id_buf); };
    EXPECT_GE(H5O_debug_id(H5O_STAB_ID, src_oloc->file, &orig_stab, id_stream,
                           2, 24),
              0);
    std::fclose(id_stream);
    const std::string id_out(id_buf != nullptr ? id_buf : "", id_len);
    EXPECT_THAT(id_out, HasSubstr("B-tree address:"));
    EXPECT_THAT(id_out, HasSubstr("Name heap address:"));
  }
  src_grp.reset();

  // Deep H5Ocopy exercises copy_file and post_copy_file including B-tree walk.
  ScopedFile dst_file(
      H5Fcreate("stab_dst.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(dst_file.valid());
  ASSERT_GE(H5Ocopy(src_file.get(), "/v1_grp", dst_file.get(), "/copied_full",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedDataset copied_leaf(
      H5Dopen2(dst_file.get(), "/copied_full/sub/leaf", H5P_DEFAULT));
  ASSERT_TRUE(copied_leaf.valid());
  int read_val = 0;
  ASSERT_GE(H5Dread(copied_leaf.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, &read_val),
            0);
  EXPECT_EQ(read_val, val);
  copied_leaf.reset();

  // Shallow H5Ocopy (max_depth = 1) copies immediate child group `sub` but skips
  // `sub`'s children in H5O__stab_post_copy_file (`leaf` is omitted).
  ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpypl.valid());
  ASSERT_GE(H5Pset_copy_object(ocpypl.get(), H5O_COPY_SHALLOW_HIERARCHY_FLAG),
            0);
  ASSERT_GE(H5Ocopy(src_file.get(), "/v1_grp", dst_file.get(),
                    "/copied_shallow", ocpypl.get(), H5P_DEFAULT),
            0);
  EXPECT_GT(H5Lexists(dst_file.get(), "/copied_shallow/sub", H5P_DEFAULT), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_FALSE(
        H5Lexists(dst_file.get(), "/copied_shallow/sub/leaf", H5P_DEFAULT) > 0);
  }

  // Delete an empty v1 group so H5O__stab_delete runs end-to-end.
  ScopedGroup empty_v1(
      H5Gcreate2(dst_file.get(), "/empty_v1", H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT));
  ASSERT_TRUE(empty_v1.valid());
  empty_v1.reset();
  ASSERT_GE(H5Ldelete(dst_file.get(), "/empty_v1", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(dst_file.get(), "/empty_v1", H5P_DEFAULT), 0);
}

TEST(H5OstabTest, DeleteCopyFileAndPostCopyFileErrorPaths) {
  ScopedPlist fapl = MakeEarliestCoreFapl();
  ASSERT_TRUE(fapl.valid());

  ScopedFile src_file(
      H5Fcreate("stab_err_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(src_file.valid());
  ScopedGroup grp(
      H5Gcreate2(src_file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedGroup sub(
      H5Gcreate2(grp.get(), "child", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub.valid());
  sub.reset();
  ASSERT_GE(H5Fflush(src_file.get(), H5F_SCOPE_GLOBAL), 0);

  H5O_loc_t* src_oloc = H5O_get_loc(grp.get());
  ASSERT_NE(src_oloc, nullptr);
  H5F_t* f_src = src_oloc->file;

  // Create a second EARLIEST file, flush, and reopen as read-only via image so
  // H5G__stab_create_components fails deterministically in copy_file.
  ScopedFile tmp_dst(
      H5Fcreate("stab_err_dst_w.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(tmp_dst.valid());
  ASSERT_GE(H5Fflush(tmp_dst.get(), H5F_SCOPE_GLOBAL), 0);
  const ssize_t dst_len = H5Fget_file_image(tmp_dst.get(), nullptr, 0);
  ASSERT_GT(dst_len, 0);
  std::vector<uint8_t> dst_img(static_cast<size_t>(dst_len));
  ASSERT_EQ(H5Fget_file_image(tmp_dst.get(), dst_img.data(), dst_img.size()),
            dst_len);
  tmp_dst.reset();

  ScopedPlist ro_fapl = MakeEarliestCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), dst_img.data(), dst_img.size()),
            0);
  ScopedFile ro_dst(H5Fopen("stab_err_dst_ro.h5", H5F_ACC_RDONLY,
                            ro_fapl.get()));
  ASSERT_TRUE(ro_dst.valid());
  H5F_t* f_dst_ro = UnwrapFile(ro_dst.get());
  ASSERT_NE(f_dst_ro, nullptr);

  ScopedApiContext ctx(H5AC__IGNORE_TAG);
  ASSERT_TRUE(ctx.valid());

  H5O_stab_t real_stab{};
  ASSERT_NE(H5O_msg_read(src_oloc, H5O_STAB_ID, &real_stab), nullptr);

  ScopedErrorSilence silence;

  // 1. del failure when H5G__stab_delete fails on bogus addresses.
  H5O_stab_t bad_stab{0xdeadbeef00ULL, 0xdeadbeef00ULL};
  EXPECT_LT(H5O_msg_delete(f_src, nullptr, H5O_STAB_ID, &bad_stab), 0);

  // 2. copy_file error: H5HL_get_size fails on invalid source heap_addr.
  bool recompute = false;
  unsigned mesg_flags = 0;
  H5O_copy_t cpy_info{};
  cpy_info.max_depth = -1;
  H5G_copy_file_ud_t udata{};
  ASSERT_NE(H5O_MSG_STAB[0].copy_file, nullptr);
  EXPECT_EQ(H5O_MSG_STAB[0].copy_file(f_src, &bad_stab, f_dst_ro, &recompute,
                                      &mesg_flags, &cpy_info, &udata),
            nullptr);

  // 3. copy_file error: valid src stab, H5G__stab_create_components fails on
  //    read-only destination file -> frees allocated stab_dst cleanly.
  EXPECT_EQ(H5O_MSG_STAB[0].copy_file(f_src, &real_stab, f_dst_ro, &recompute,
                                      &mesg_flags, &cpy_info, &udata),
            nullptr);

  // 4. post_copy_file error: H5HL_protect fails on invalid heap_addr.
  H5O_loc_t dst_oloc{};
  dst_oloc.file = f_dst_ro;
  dst_oloc.addr = 0x100;
  H5O_stab_t dst_stab{};
  ASSERT_NE(H5O_MSG_STAB[0].post_copy_file, nullptr);
  EXPECT_LT(H5O_MSG_STAB[0].post_copy_file(src_oloc, &bad_stab, &dst_oloc,
                                           &dst_stab, &mesg_flags, &cpy_info),
            0);

  // 5. post_copy_file error: valid heap_addr (H5HL_protect succeeds), bogus
  //    btree_addr (H5B_iterate fails), and done unprotects heap cleanly.
  H5O_stab_t bad_btree_stab = real_stab;
  bad_btree_stab.btree_addr = 0xdeadbeef00ULL;
  EXPECT_LT(H5O_MSG_STAB[0].post_copy_file(src_oloc, &bad_btree_stab, &dst_oloc,
                                           &dst_stab, &mesg_flags, &cpy_info),
            0);
}

}  // namespace
}  // namespace hdf5
