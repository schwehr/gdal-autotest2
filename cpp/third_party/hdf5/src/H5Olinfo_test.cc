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
// Tests link info object header messages: encode/decode, raw size, copy,
// deletion of dense link storage, cross-file & shallow H5Ocopy behavior,
// creation order tracking/indexing, and debug output
// (third_party/hdf5/src/H5Olinfo.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Olinfo.c

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/string_view.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields in cache headers and
// object headers pulled in via `H5Opkg.h`.
#ifndef NDEBUG
#define H5O_LINFO_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5O_LINFO_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5O_LINFO_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__COPIED_TAG) {
    std::memset(&node_, 0, sizeof(node_));
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(tag, &prev_tag_);
      tagged_ = true;
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
  H5CX_node_t node_;
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

H5F_t* GetNativeFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

std::string ReadWholeFile(const TempFile& file) {
  std::FILE* fp = std::fopen(file.c_str(), "rb");
  if (fp == nullptr) {
    return "";
  }
  absl::Cleanup close_fp = [fp] { std::fclose(fp); };
  std::string out;
  char buf[512];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
    out.append(buf, n);
  }
  return out;
}

TEST(H5OlinfoTest, MessageClassMetadataMatches) {
  EXPECT_EQ(H5O_MSG_LINFO[0].id, H5O_LINFO_ID);
  EXPECT_STREQ(H5O_MSG_LINFO[0].name, "linfo");
  EXPECT_EQ(H5O_MSG_LINFO[0].native_size, sizeof(H5O_linfo_t));
}

TEST(H5OlinfoTest, EncodeDecodeCopySizeAndFreeRoundTrips) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);
  ScopedFile file(
      H5Fcreate("linfo_rt.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetNativeFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  struct Case {
    bool track_corder;
    bool index_corder;
    int64_t max_corder;
    haddr_t fheap_addr;
    haddr_t name_bt2_addr;
    haddr_t corder_bt2_addr;
  };
  const Case cases[] = {
      {false, false, 0, HADDR_UNDEF, HADDR_UNDEF, HADDR_UNDEF},
      {true, false, 0, 0x1200, 0x2400, HADDR_UNDEF},
      {true, true, 12345, 0x3400, 0x4500, 0x5600},
      {false, true, 0, 0x6000, 0x7000, 0x8000},
  };

  for (const auto& tc : cases) {
    H5O_linfo_t src{};
    src.track_corder = tc.track_corder;
    src.index_corder = tc.index_corder;
    src.max_corder = tc.max_corder;
    src.nlinks = 7;
    src.fheap_addr = tc.fheap_addr;
    src.name_bt2_addr = tc.name_bt2_addr;
    src.corder_bt2_addr = tc.corder_bt2_addr;

    const size_t raw_size = H5O_msg_raw_size(f, H5O_LINFO_ID, false, &src);
    const size_t expected_size =
        2 + (tc.track_corder ? 8 : 0) + 2 * H5F_SIZEOF_ADDR(f) +
        (tc.index_corder ? H5F_SIZEOF_ADDR(f) : 0);
    EXPECT_EQ(raw_size, expected_size);

    std::vector<uint8_t> encoded(raw_size, 0);
    ASSERT_GE(H5O_msg_encode(f, H5O_LINFO_ID, false, encoded.data(), &src), 0);

    H5O_linfo_t* decoded = static_cast<H5O_linfo_t*>(
        H5O_msg_decode(f, nullptr, H5O_LINFO_ID, encoded.size(), encoded.data()));
    ASSERT_NE(decoded, nullptr);
    absl::Cleanup free_decoded = [decoded] {
      H5O_msg_free(H5O_LINFO_ID, decoded);
    };

    EXPECT_EQ(decoded->track_corder, tc.track_corder);
    EXPECT_EQ(decoded->index_corder, tc.index_corder);
    EXPECT_EQ(decoded->max_corder, tc.track_corder ? tc.max_corder : 0);
    EXPECT_EQ(decoded->nlinks, HSIZET_MAX);
    EXPECT_EQ(decoded->fheap_addr, tc.fheap_addr);
    EXPECT_EQ(decoded->name_bt2_addr, tc.name_bt2_addr);
    EXPECT_EQ(decoded->corder_bt2_addr, tc.corder_bt2_addr);

    H5O_linfo_t* copied_alloc = static_cast<H5O_linfo_t*>(
        H5O_msg_copy(H5O_LINFO_ID, decoded, nullptr));
    ASSERT_NE(copied_alloc, nullptr);
    absl::Cleanup free_copied = [copied_alloc] {
      H5O_msg_free(H5O_LINFO_ID, copied_alloc);
    };
    EXPECT_EQ(copied_alloc->track_corder, decoded->track_corder);
    EXPECT_EQ(copied_alloc->max_corder, decoded->max_corder);
    EXPECT_EQ(copied_alloc->corder_bt2_addr, decoded->corder_bt2_addr);

    H5O_linfo_t copied_inplace{};
    ASSERT_EQ(H5O_msg_copy(H5O_LINFO_ID, &src, &copied_inplace),
              &copied_inplace);
    EXPECT_EQ(copied_inplace.fheap_addr, src.fheap_addr);
    EXPECT_EQ(copied_inplace.name_bt2_addr, src.name_bt2_addr);
    EXPECT_EQ(copied_inplace.corder_bt2_addr, src.corder_bt2_addr);
  }
}

TEST(H5OlinfoTest, DecodeRejectsMalformedAndTruncatedBuffers) {
  ScopedFile file = CreateInMemoryFile("linfo_bad_decode.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetNativeFile(file.get());
  ASSERT_NE(f, nullptr);
  const size_t addr_size = H5F_SIZEOF_ADDR(f);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  // Truncated before 2-byte header.
  const std::vector<uint8_t> too_short = {0};
  EXPECT_EQ(
      H5O_msg_decode(f, nullptr, H5O_LINFO_ID, too_short.size(), too_short.data()),
      nullptr);

  // Bad version number.
  std::vector<uint8_t> bad_version(2 + 2 * addr_size, 0);
  bad_version[0] = 9;
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINFO_ID, bad_version.size(),
                           bad_version.data()),
            nullptr);

  // Unsupported flag bits.
  std::vector<uint8_t> bad_flags(2 + 2 * addr_size, 0);
  bad_flags[1] = 0x80;
  EXPECT_EQ(
      H5O_msg_decode(f, nullptr, H5O_LINFO_ID, bad_flags.size(), bad_flags.data()),
      nullptr);

  // track_corder set, truncated 8-byte max_corder.
  const std::vector<uint8_t> short_corder = {0, 0x01, 0, 0, 0};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINFO_ID, short_corder.size(),
                           short_corder.data()),
            nullptr);

  // track_corder set with negative max_corder (-1 in big-endian).
  std::vector<uint8_t> neg_corder(10 + 2 * addr_size, 0);
  neg_corder[1] = 0x01;
  std::memset(&neg_corder[2], 0xff, 8);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINFO_ID, neg_corder.size(),
                           neg_corder.data()),
            nullptr);

  // Truncated fheap/name_bt2 addresses.
  std::vector<uint8_t> short_addrs(2 + addr_size, 0);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINFO_ID, short_addrs.size(),
                           short_addrs.data()),
            nullptr);

  // index_corder set, truncated corder_bt2_addr.
  std::vector<uint8_t> short_corder_bt2(2 + 2 * addr_size, 0);
  short_corder_bt2[1] = 0x02;
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LINFO_ID, short_corder_bt2.size(),
                           short_corder_bt2.data()),
            nullptr);
}

TEST(H5OlinfoTest, DeleteHandlesUndefinedAndInvalidDenseAddresses) {
  ScopedFile file = CreateInMemoryFile("linfo_delete.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetNativeFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5O_linfo_t compact{};
  compact.fheap_addr = HADDR_UNDEF;
  compact.name_bt2_addr = HADDR_UNDEF;
  compact.corder_bt2_addr = HADDR_UNDEF;
  EXPECT_GE(H5O_msg_delete(f, nullptr, H5O_LINFO_ID, &compact), 0);

  ScopedErrorSilence silence;
  H5O_linfo_t bad_dense{};
  bad_dense.fheap_addr = 0xdeadbeef00ULL;
  bad_dense.name_bt2_addr = 0xdeadbef000ULL;
  bad_dense.corder_bt2_addr = HADDR_UNDEF;
  EXPECT_LT(H5O_msg_delete(f, nullptr, H5O_LINFO_ID, &bad_dense), 0);
}

TEST(H5OlinfoTest, DebugPrintsCompactAndDenseVariants) {
  ScopedFile file = CreateInMemoryFile("linfo_debug.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetNativeFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  TempFile dbg_file("linfo_debug.txt");
  std::FILE* fp = std::fopen(dbg_file.c_str(), "wb");
  ASSERT_NE(fp, nullptr);

  {
    absl::Cleanup close_fp = [&fp] {
      if (fp != nullptr) {
        std::fclose(fp);
      }
    };

    H5O_linfo_t compact{};
    compact.track_corder = false;
    compact.index_corder = false;
    compact.nlinks = 3;
    compact.max_corder = 0;
    compact.fheap_addr = HADDR_UNDEF;
    compact.name_bt2_addr = HADDR_UNDEF;
    compact.corder_bt2_addr = HADDR_UNDEF;
    EXPECT_GE(H5O_debug_id(H5O_LINFO_ID, f, &compact, fp, 0, 40), 0);

    H5O_linfo_t dense{};
    dense.track_corder = true;
    dense.index_corder = true;
    dense.nlinks = 25;
    dense.max_corder = 42;
    dense.fheap_addr = 0x1000;
    dense.name_bt2_addr = 0x2000;
    dense.corder_bt2_addr = 0x3000;
    EXPECT_GE(H5O_debug_id(H5O_LINFO_ID, f, &dense, fp, 2, 40), 0);

    std::fclose(fp);
    fp = nullptr;
  }

  const std::string output = ReadWholeFile(dbg_file);
  EXPECT_THAT(output, HasSubstr("Track creation order of links:"));
  EXPECT_THAT(output, HasSubstr("FALSE"));
  EXPECT_THAT(output, HasSubstr("TRUE"));
  EXPECT_THAT(output, HasSubstr("42"));
}

TEST(H5OlinfoTest, PublicDenseGroupCopyShallowCopyAndDeletion) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);

  ScopedFile src(
      H5Fcreate("linfo_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ScopedFile dst(
      H5Fcreate("linfo_dst.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  // Force dense storage immediately (0 max compact links).
  ASSERT_GE(H5Pset_link_phase_change(gcpl.get(), 0, 0), 0);
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);

  {
    ScopedGroup parent(
        H5Gcreate2(src.get(), "/parent", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(parent.valid());

    // Create child groups: one dense with links and one compact.
    ScopedGroup dense_child(H5Gcreate2(parent.get(), "dense_child", H5P_DEFAULT,
                                       gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(dense_child.valid());
    for (int i = 0; i < 6; ++i) {
      const std::string name = "leaf_" + std::to_string(i);
      ScopedGroup leaf(H5Gcreate2(dense_child.get(), name.c_str(), H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(leaf.valid());
    }
    ASSERT_GE(H5Lcreate_soft("/target", dense_child.get(), "soft_link",
                             H5P_DEFAULT, H5P_DEFAULT),
              0);

    ScopedGroup compact_child(H5Gcreate2(parent.get(), "compact_child",
                                         H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(compact_child.valid());
    ScopedGroup nested(H5Gcreate2(compact_child.get(), "nested", H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(nested.valid());

    H5G_info_t ginfo{};
    ASSERT_GE(H5Gget_info(dense_child.get(), &ginfo), 0);
    EXPECT_EQ(ginfo.storage_type, H5G_STORAGE_TYPE_DENSE);
  }

  // Full copy exercises dense copy_file, post_copy_file, and post_copy_file_cb.
  ASSERT_GE(
      H5Ocopy(src.get(), "/parent", dst.get(), "/full_copy", H5P_DEFAULT, H5P_DEFAULT),
      0);

  H5G_info_t dst_info{};
  ASSERT_GE(H5Gget_info_by_name(dst.get(), "/full_copy/dense_child", &dst_info,
                                H5P_DEFAULT),
            0);
  EXPECT_EQ(dst_info.storage_type, H5G_STORAGE_TYPE_DENSE);
  EXPECT_EQ(dst_info.nlinks, 7);

  // Shallow copy of /parent hits depth limit (curr_depth >= max_depth) inside
  // child groups (both dense_child and compact_child) in copy_file &
  // post_copy_file.
  ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpypl.valid());
  ASSERT_GE(H5Pset_copy_object(ocpypl.get(), H5O_COPY_SHALLOW_HIERARCHY_FLAG),
            0);
  ASSERT_GE(H5Ocopy(src.get(), "/parent", dst.get(), "/shallow_copy",
                    ocpypl.get(), H5P_DEFAULT),
            0);

  H5G_info_t shallow_dense_info{};
  ASSERT_GE(H5Gget_info_by_name(dst.get(), "/shallow_copy/dense_child",
                                &shallow_dense_info, H5P_DEFAULT),
            0);
  EXPECT_EQ(shallow_dense_info.nlinks, 0);

  // Unlink dense group in src to exercise H5O__linfo_delete -> H5G__dense_delete.
  ASSERT_GE(H5Ldelete(src.get(), "/parent/dense_child", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(src.get(), "/parent", H5P_DEFAULT), 0);
}

}  // namespace
}  // namespace hdf5
