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
// Tests Object Header Reference Count messages (encoding, decoding, copying,
// sizing, freeing, cross-file pre-copy deletion, debug formatting, and
// multi-link/reopen lifecycles) in third_party/hdf5/src/H5Orefcount.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Orefcount.c

#include <stdio.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#define H5OREFCOUNT_TEST_RESTORE_UNDEF_NDEBUG 1
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
#define H5O_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5OREFCOUNT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OREFCOUNT_TEST_RESTORE_UNDEF_NDEBUG
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

std::string CaptureDebugId(H5F_t* f, const H5O_refcount_t* rc, int indent,
                           int fwidth) {
  char* buf = nullptr;
  size_t len = 0;
  FILE* stream = open_memstream(&buf, &len);
  if (stream == nullptr) {
    return "";
  }
  absl::Cleanup free_buf = [&] { std::free(buf); };
  const herr_t status =
      H5O_debug_id(H5O_REFCOUNT_ID, f, rc, stream, indent, fwidth);
  std::fclose(stream);
  if (status < 0) {
    return "";
  }
  return std::string(buf != nullptr ? buf : "", len);
}

TEST(H5OrefcountTest, EncodeDecodeSizeCopyFreeRoundTrips) {
  ScopedFile file = CreateInMemoryFile("refcount_roundtrip.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  for (const H5O_refcount_t expected : {0u, 1u, 2u, 42u, 0x12345678u,
                                        0xffffffffu}) {
    const size_t raw_sz =
        H5O_msg_raw_size(f, H5O_REFCOUNT_ID, /*disable_shared=*/false,
                         &expected);
    EXPECT_EQ(raw_sz, 5u);

    std::vector<uint8_t> encoded(raw_sz, 0xAAu);
    ASSERT_GE(H5O_msg_encode(f, H5O_REFCOUNT_ID, /*disable_shared=*/false,
                             encoded.data(), &expected),
              0);
    EXPECT_EQ(encoded[0], 0u);
    EXPECT_EQ(encoded[1], static_cast<uint8_t>(expected & 0xffu));
    EXPECT_EQ(encoded[2], static_cast<uint8_t>((expected >> 8) & 0xffu));
    EXPECT_EQ(encoded[3], static_cast<uint8_t>((expected >> 16) & 0xffu));
    EXPECT_EQ(encoded[4], static_cast<uint8_t>((expected >> 24) & 0xffu));

    auto* decoded = static_cast<H5O_refcount_t*>(H5O_msg_decode(
        f, nullptr, H5O_REFCOUNT_ID, encoded.size(), encoded.data()));
    ASSERT_NE(decoded, nullptr);
    absl::Cleanup free_decoded = [&] {
      H5O_msg_free(H5O_REFCOUNT_ID, decoded);
    };
    EXPECT_EQ(*decoded, expected);

    // Copy into caller-provided buffer.
    H5O_refcount_t copy_inplace = 0;
    void* copy_ret = H5O_msg_copy(H5O_REFCOUNT_ID, decoded, &copy_inplace);
    EXPECT_EQ(copy_ret, &copy_inplace);
    EXPECT_EQ(copy_inplace, expected);

    // Copy with null destination allocates a new H5O_refcount_t.
    auto* copy_alloc = static_cast<H5O_refcount_t*>(
        H5O_msg_copy(H5O_REFCOUNT_ID, decoded, nullptr));
    ASSERT_NE(copy_alloc, nullptr);
    absl::Cleanup free_copy = [&] {
      H5O_msg_free(H5O_REFCOUNT_ID, copy_alloc);
    };
    EXPECT_EQ(*copy_alloc, expected);
  }
}

TEST(H5OrefcountTest, DecodeRejectsOverflowAndBadVersion) {
  ScopedFile file = CreateInMemoryFile("refcount_decode_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  // Empty buffer (p_size == 0) hits version overflow check.
  std::array<uint8_t, 2> dummy{0u, 0u};
  EXPECT_EQ(
      H5O_msg_decode(f, nullptr, H5O_REFCOUNT_ID, 0, dummy.data() + 1),
      nullptr);

  // Bad version numbers (!= 0).
  const std::array<uint8_t, 5> bad_v1 = {1u, 2u, 0u, 0u, 0u};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_REFCOUNT_ID, bad_v1.size(),
                           bad_v1.data()),
            nullptr);
  const std::array<uint8_t, 5> bad_v255 = {255u, 2u, 0u, 0u, 0u};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_REFCOUNT_ID, bad_v255.size(),
                           bad_v255.data()),
            nullptr);

  // Truncated after valid version byte (allocates refcount, hits 4-byte count
  // overflow check, and frees refcount cleanly).
  const std::array<uint8_t, 5> truncated = {0u, 3u, 0u, 0u, 0u};
  for (size_t trunc_len : {1u, 2u, 3u, 4u}) {
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_REFCOUNT_ID, trunc_len,
                             truncated.data()),
              nullptr);
  }
}

TEST(H5OrefcountTest, DebugFormatsLinkCount) {
  ScopedFile file = CreateInMemoryFile("refcount_debug.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  const H5O_refcount_t rc = 7;
  const std::string out = CaptureDebugId(f, &rc, /*indent=*/2, /*fwidth=*/20);
  EXPECT_THAT(out, HasSubstr("Number of links:"));
  EXPECT_THAT(out, HasSubstr("7"));
}

TEST(H5OrefcountTest, PreCopyFileMarksDeletedAndLifecycleAcrossReopen) {
  // Latest-format objects store H5O_REFCOUNT_ID whenever link count > 1.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);

  ScopedFile src_file(
      H5Fcreate("refcount_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(src_file.valid());

  ScopedGroup grp(
      H5Gcreate2(src_file.get(), "/obj", H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  H5O_loc_t* obj_oloc = H5O_get_loc(grp.get());
  ASSERT_NE(obj_oloc, nullptr);
  ASSERT_NE(obj_oloc->file, nullptr);

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    // Refcount is 1: H5O_REFCOUNT_ID is not yet present.
    EXPECT_EQ(H5O_msg_exists(obj_oloc, H5O_REFCOUNT_ID), 0);
  }

  // Hard link raises refcount to 2 -> H5O_REFCOUNT_ID created.
  ASSERT_GE(H5Lcreate_hard(src_file.get(), "/obj", src_file.get(), "/obj_link",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  // Explicit H5Oincr_refcount raises refcount to 3 -> H5O_REFCOUNT_ID updated.
  ASSERT_GE(H5Oincr_refcount(grp.get()), 0);

  H5O_info2_t oinfo{};
  ASSERT_GE(H5Oget_info3(grp.get(), &oinfo, H5O_INFO_BASIC), 0);
  EXPECT_EQ(oinfo.rc, 3u);

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    EXPECT_EQ(H5O_msg_exists(obj_oloc, H5O_REFCOUNT_ID), 1);

    H5O_refcount_t read_rc = 0;
    ASSERT_NE(H5O_msg_read(obj_oloc, H5O_REFCOUNT_ID, &read_rc), nullptr);
    EXPECT_EQ(read_rc, 3u);

    // Direct check of pre_copy_file callback marking the message deleted.
    bool deleted = false;
    H5O_copy_t cpy_info{};
    ASSERT_NE(H5O_MSG_REFCOUNT[0].pre_copy_file, nullptr);
    EXPECT_GE(H5O_MSG_REFCOUNT[0].pre_copy_file(obj_oloc->file, &read_rc,
                                                &deleted, &cpy_info, nullptr),
              0);
    EXPECT_TRUE(deleted);
  }

  // Flush and reopen from in-memory image to exercise metadata cache decode and
  // full H5O_debug dump of the object header containing H5O_REFCOUNT_ID.
  ASSERT_GE(H5Fflush(src_file.get(), H5F_SCOPE_GLOBAL), 0);
  const ssize_t img_len = H5Fget_file_image(src_file.get(), nullptr, 0);
  ASSERT_GT(img_len, 0);
  std::vector<uint8_t> image(static_cast<size_t>(img_len));
  ASSERT_EQ(H5Fget_file_image(src_file.get(), image.data(), image.size()),
            img_len);

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(H5Fopen("ro_snap.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());

  ScopedGroup ro_grp(H5Gopen2(ro_file.get(), "/obj", H5P_DEFAULT));
  ASSERT_TRUE(ro_grp.valid());
  H5O_info2_t ro_info{};
  ASSERT_GE(H5Oget_info3(ro_grp.get(), &ro_info, H5O_INFO_BASIC), 0);
  EXPECT_EQ(ro_info.rc, 3u);

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_loc_t* ro_oloc = H5O_get_loc(ro_grp.get());
    ASSERT_NE(ro_oloc, nullptr);
    char* dbg_buf = nullptr;
    size_t dbg_len = 0;
    FILE* stream = open_memstream(&dbg_buf, &dbg_len);
    ASSERT_NE(stream, nullptr);
    absl::Cleanup free_dbg = [&] { std::free(dbg_buf); };
    EXPECT_GE(H5O_debug(ro_oloc->file, ro_oloc->addr, stream, 0, 24), 0);
    std::fclose(stream);
    const std::string dbg(dbg_buf != nullptr ? dbg_buf : "", dbg_len);
    EXPECT_THAT(dbg, HasSubstr("Number of links:"));
    EXPECT_THAT(dbg, HasSubstr("3"));
  }
  ro_grp.reset();
  ro_file.reset();

  // Cross-file H5Ocopy runs H5O__refcount_pre_copy_file so the copy starts at
  // rc == 1 without copying the source's rc == 3 message.
  ScopedFile dst_file(
      H5Fcreate("refcount_dst.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(dst_file.valid());
  ASSERT_GE(H5Ocopy(src_file.get(), "/obj", dst_file.get(), "/copied_obj",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  ScopedGroup copied_grp(H5Gopen2(dst_file.get(), "/copied_obj", H5P_DEFAULT));
  ASSERT_TRUE(copied_grp.valid());
  H5O_info2_t copied_info{};
  ASSERT_GE(H5Oget_info3(copied_grp.get(), &copied_info, H5O_INFO_BASIC), 0);
  EXPECT_EQ(copied_info.rc, 1u);
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_loc_t* copied_oloc = H5O_get_loc(copied_grp.get());
    ASSERT_NE(copied_oloc, nullptr);
    EXPECT_EQ(H5O_msg_exists(copied_oloc, H5O_REFCOUNT_ID), 0);
  }

  // Drop refcount back down to 1 -> H5O_REFCOUNT_ID is removed.
  ASSERT_GE(H5Odecr_refcount(grp.get()), 0);
  ASSERT_GE(H5Ldelete(src_file.get(), "/obj_link", H5P_DEFAULT), 0);
  ASSERT_GE(H5Oget_info3(grp.get(), &oinfo, H5O_INFO_BASIC), 0);
  EXPECT_EQ(oinfo.rc, 1u);
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    EXPECT_EQ(H5O_msg_exists(obj_oloc, H5O_REFCOUNT_ID), 0);
  }
}

}  // namespace
}  // namespace hdf5
