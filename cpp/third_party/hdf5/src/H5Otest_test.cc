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
// Tests internal H5O_TESTING object-header inspection, attribute storage,
// unknown-message marking, cache expunge, refcount, and chunk-manipulation
// helpers (third_party/hdf5/src/H5Otest.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Otest.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits debug fields in H5O_t and
// H5C_cache_entry_t. Define NDEBUG around `hdf5_internal_prelude.h` and the
// internal H5O headers so struct field offsets match the compiled library.
#ifndef NDEBUG
#define H5OTEST_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5F_FRIEND
#define H5O_TESTING
#define H5O_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
// `H5Fpkg.h` declares `H5F__efc_open(bool try, ...)`, which collides with
// Google3's STL `#define try if (1)` under -fno-exceptions.
#pragma push_macro("try")
#ifdef try
#undef try
#define try hdf5_c_param_try
#endif
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5Opkg.h"
}  // extern "C"
#ifdef H5OTEST_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OTEST_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

constexpr char kGroupName[] = "/obj";
constexpr char kDatasetName[] = "/dset";
constexpr char kFirstAttrName[] = "a_first";
constexpr char kSecondAttrName[] = "b_second";
constexpr uint8_t kSyntheticUnknownMsgId = 0xfe;

// Pushes an HDF5 API context so internal routines invoking FUNC_ENTER_*_TAG
// have a valid active context node on the thread-local stack.
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

 private:
  H5CX_node_t node_ = {{0}, nullptr};
  bool pushed_ = false;
};

bool AddInt32Attr(hid_t oid, const char* name, int32_t value) {
  ScopedSpace space(H5Screate(H5S_SCALAR));
  if (!space.valid()) return false;
  ScopedAttr attr(H5Acreate2(oid, name, H5T_NATIVE_INT32, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  if (!attr.valid()) return false;
  return H5Awrite(attr.get(), H5T_NATIVE_INT32, &value) >= 0;
}

bool AddLargeInt64ArrayAttr(hid_t oid, const char* name, hsize_t count) {
  ScopedSpace space(H5Screate_simple(1, &count, nullptr));
  if (!space.valid()) return false;
  ScopedAttr attr(H5Acreate2(oid, name, H5T_NATIVE_INT64, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  if (!attr.valid()) return false;
  std::vector<int64_t> data(static_cast<size_t>(count), 7);
  return H5Awrite(attr.get(), H5T_NATIVE_INT64, data.data()) >= 0;
}

// Locates the first message with `target_msg_id` in the v2 object header at
// `oh_addr` and calls `patch_fn(msg_hdr, payload_size)`, then recomputes chunk
// 0's metadata checksum.
template <typename PatchFn>
bool PatchV2Chunk0Message(std::vector<uint8_t>* image, haddr_t oh_addr,
                          uint8_t target_msg_id, PatchFn&& patch_fn) {
  if (oh_addr + 6 > image->size()) return false;
  uint8_t* const chunk_start = image->data() + oh_addr;
  if (std::memcmp(chunk_start, H5O_HDR_MAGIC, H5_SIZEOF_MAGIC) != 0) {
    return false;
  }
  if (chunk_start[4] != H5O_VERSION_2) return false;
  const uint8_t oh_flags = chunk_start[5];

  size_t pos = 6;
  if (oh_flags & H5O_HDR_STORE_TIMES) pos += 16;
  if (oh_flags & H5O_HDR_ATTR_STORE_PHASE_CHANGE) pos += 4;
  const unsigned size_bytes = 1U << (oh_flags & H5O_HDR_CHUNK0_SIZE);
  if (oh_addr + pos + size_bytes > image->size()) return false;

  uint64_t chunk0_size = 0;
  for (unsigned i = 0; i < size_bytes; ++i) {
    chunk0_size |= static_cast<uint64_t>(chunk_start[pos + i]) << (8U * i);
  }
  pos += size_bytes;

  const size_t msg_hdr_size =
      (oh_flags & H5O_HDR_ATTR_CRT_ORDER_TRACKED) ? 6 : 4;
  const size_t msg_end = pos + static_cast<size_t>(chunk0_size);
  const size_t total_chunk_len = msg_end + H5O_SIZEOF_CHKSUM;
  if (oh_addr + total_chunk_len > image->size()) return false;

  bool patched = false;
  size_t cur = pos;
  while (cur + msg_hdr_size <= msg_end) {
    const uint8_t msg_id = chunk_start[cur];
    const uint16_t msg_size = static_cast<uint16_t>(chunk_start[cur + 1]) |
                              (static_cast<uint16_t>(chunk_start[cur + 2]) << 8U);
    if (cur + msg_hdr_size + msg_size > msg_end) return false;
    if (msg_id == target_msg_id) {
      if (!patch_fn(chunk_start + cur, msg_hdr_size, msg_size)) return false;
      patched = true;
      break;
    }
    cur += msg_hdr_size + msg_size;
  }
  if (!patched) return false;

  const uint32_t chksum =
      H5_checksum_metadata(chunk_start, total_chunk_len - H5O_SIZEOF_CHKSUM, 0);
  uint8_t* const chksum_ptr = chunk_start + total_chunk_len - H5O_SIZEOF_CHKSUM;
  chksum_ptr[0] = static_cast<uint8_t>(chksum & 0xffU);
  chksum_ptr[1] = static_cast<uint8_t>((chksum >> 8U) & 0xffU);
  chksum_ptr[2] = static_cast<uint8_t>((chksum >> 16U) & 0xffU);
  chksum_ptr[3] = static_cast<uint8_t>((chksum >> 24U) & 0xffU);
  return true;
}

bool PatchNullMessageToUnknown(std::vector<uint8_t>* image, haddr_t oh_addr,
                               uint8_t msg_flags) {
  return PatchV2Chunk0Message(
      image, oh_addr, H5O_NULL_ID,
      [msg_flags](uint8_t* msg_hdr, size_t /*msg_hdr_size*/,
                  uint16_t /*msg_size*/) {
        msg_hdr[0] = kSyntheticUnknownMsgId;
        msg_hdr[3] = msg_flags;
        return true;
      });
}

bool PatchAinfoVersion(std::vector<uint8_t>* image, haddr_t oh_addr,
                       uint8_t bad_version) {
  return PatchV2Chunk0Message(
      image, oh_addr, H5O_AINFO_ID,
      [bad_version](uint8_t* msg_hdr, size_t msg_hdr_size, uint16_t msg_size) {
        if (msg_size < 1) return false;
        msg_hdr[msg_hdr_size] = bad_version;
        return true;
      });
}

// Updates the cached native H5O_AINFO_ID fields on `loc` via protect/unprotect
// so subsequent `H5A__get_ainfo` reads see the specified fields without altering
// the on-disk message bytes.
bool MutateCachedAinfo(const H5O_loc_t* loc, hsize_t nattrs,
                       haddr_t name_bt2_addr, haddr_t corder_bt2_addr) {
  ScopedApiContext api_ctx;
  H5O_t* oh = H5O_protect(loc, H5AC__READ_ONLY_FLAG, false);
  if (oh == nullptr) return false;
  bool updated = false;
  for (size_t i = 0; i < oh->nmesgs; ++i) {
    if (oh->mesg[i].type->id == H5O_AINFO_ID && oh->mesg[i].native != nullptr) {
      auto* ainfo = static_cast<H5O_ainfo_t*>(oh->mesg[i].native);
      ainfo->nattrs = nattrs;
      ainfo->name_bt2_addr = name_bt2_addr;
      ainfo->corder_bt2_addr = corder_bt2_addr;
      updated = true;
      break;
    }
  }
  const herr_t unprot_rc = H5O_unprotect(loc, oh, H5AC__NO_FLAGS_SET);
  return updated && (unprot_rc >= 0);
}

TEST(H5OtestTest, AttributeStorageQueriesHandleV1V2CompactAndDense) {
  // 1. Test v1 (earliest format) object headers: never dense, count tracks
  // attribute creation and deletion accurately, and dense_info_test fails.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
        0);
    // Low bound EARLIEST uses v1 object headers by default.
    ScopedFile v1_file(
        H5Fcreate("v1_attrs.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(v1_file.valid());
    ScopedGroup v1_grp(H5Gcreate2(v1_file.get(), kGroupName, H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(v1_grp.valid());

    EXPECT_EQ(H5O__is_attr_dense_test(v1_grp.get()), 0);
    EXPECT_GT(H5O__is_attr_empty_test(v1_grp.get()), 0);

    hsize_t nattrs = 999;
    ASSERT_GE(H5O__num_attrs_test(v1_grp.get(), &nattrs), 0);
    EXPECT_EQ(nattrs, 0U);

    ASSERT_TRUE(AddInt32Attr(v1_grp.get(), "a0", 1));
    ASSERT_TRUE(AddInt32Attr(v1_grp.get(), "a1", 2));
    EXPECT_EQ(H5O__is_attr_empty_test(v1_grp.get()), 0);
    ASSERT_GE(H5O__num_attrs_test(v1_grp.get(), &nattrs), 0);
    EXPECT_EQ(nattrs, 2U);

    hsize_t name_cnt = 999;
    hsize_t corder_cnt = 999;
    ScopedErrorSilence silence;
    EXPECT_LT(H5O__attr_dense_info_test(v1_grp.get(), &name_cnt, &corder_cnt),
              0);
  }

  // 2. Test v2 compact -> dense transition without creation-order indexing.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_V18), 0);

    ScopedFile file(
        H5Fcreate("v2_dense_no_corder.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 2, 1), 0);

    ScopedGroup grp(H5Gcreate2(file.get(), kGroupName, H5P_DEFAULT, gcpl.get(),
                               H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    // Initially no ainfo message is present (ainfo_exists == false).
    EXPECT_EQ(H5O__is_attr_dense_test(grp.get()), 0);
    EXPECT_GT(H5O__is_attr_empty_test(grp.get()), 0);
    hsize_t nattrs = 999;
    ASSERT_GE(H5O__num_attrs_test(grp.get(), &nattrs), 0);
    EXPECT_EQ(nattrs, 0U);

    // Compact storage (<= 2 attrs): still non-dense.
    ASSERT_TRUE(AddInt32Attr(grp.get(), "c0", 10));
    EXPECT_EQ(H5O__is_attr_dense_test(grp.get()), 0);
    EXPECT_EQ(H5O__is_attr_empty_test(grp.get()), 0);
    ASSERT_GE(H5O__num_attrs_test(grp.get(), &nattrs), 0);
    EXPECT_EQ(nattrs, 1U);

    {
      hsize_t name_cnt = 999;
      hsize_t corder_cnt = 999;
      ScopedErrorSilence silence;
      EXPECT_LT(H5O__attr_dense_info_test(grp.get(), &name_cnt, &corder_cnt), 0);
    }

    // Exceed max_compact (2) -> triggers dense attribute storage without
    // creation-order B-tree index (`corder_count == 0` branch).
    ASSERT_TRUE(AddInt32Attr(grp.get(), "c1", 11));
    ASSERT_TRUE(AddInt32Attr(grp.get(), "c2", 12));
    EXPECT_GT(H5O__is_attr_dense_test(grp.get()), 0);
    EXPECT_EQ(H5O__is_attr_empty_test(grp.get()), 0);
    ASSERT_GE(H5O__num_attrs_test(grp.get(), &nattrs), 0);
    EXPECT_EQ(nattrs, 3U);

    hsize_t name_cnt = 0;
    hsize_t corder_cnt = 999;
    ASSERT_GE(H5O__attr_dense_info_test(grp.get(), &name_cnt, &corder_cnt), 0);
    EXPECT_EQ(name_cnt, 3U);
    EXPECT_EQ(corder_cnt, 0U);

    // Deleting all attributes returns to compact and empty.
    ASSERT_GE(H5Adelete(grp.get(), "c0"), 0);
    ASSERT_GE(H5Adelete(grp.get(), "c1"), 0);
    ASSERT_GE(H5Adelete(grp.get(), "c2"), 0);
    EXPECT_EQ(H5O__is_attr_dense_test(grp.get()), 0);
    EXPECT_GT(H5O__is_attr_empty_test(grp.get()), 0);
  }

  // 3. Test dense storage WITH creation-order tracking and indexing.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
        0);

    ScopedFile file(H5Fcreate("v2_dense_corder.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 1, 1), 0);
    ASSERT_GE(H5Pset_attr_creation_order(
                  gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
              0);

    ScopedGroup grp(H5Gcreate2(file.get(), kGroupName, H5P_DEFAULT, gcpl.get(),
                               H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    for (int i = 0; i < 4; ++i) {
      const std::string name = absl::StrCat("attr_", i);
      ASSERT_TRUE(AddInt32Attr(grp.get(), name.c_str(), i));
    }

    EXPECT_GT(H5O__is_attr_dense_test(grp.get()), 0);
    EXPECT_EQ(H5O__is_attr_empty_test(grp.get()), 0);

    hsize_t nattrs = 0;
    ASSERT_GE(H5O__num_attrs_test(grp.get(), &nattrs), 0);
    EXPECT_EQ(nattrs, 4U);

    hsize_t name_cnt = 0;
    hsize_t corder_cnt = 0;
    ASSERT_GE(H5O__attr_dense_info_test(grp.get(), &name_cnt, &corder_cnt), 0);
    EXPECT_EQ(name_cnt, 4U);
    EXPECT_EQ(corder_cnt, 4U);
  }
}

TEST(H5OtestTest, AttributeAndChunkQueriesRejectInvalidIdsAndUndefinedAddr) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  hsize_t nattrs = 0;
  hsize_t name_cnt = 0;
  hsize_t corder_cnt = 0;
  unsigned chunkno = 0;

  // Invalid hid_t paths -> H5O_get_loc fails cleanly.
  EXPECT_LT(H5O__is_attr_dense_test(H5I_INVALID_HID), 0);
  EXPECT_LT(H5O__is_attr_empty_test(H5I_INVALID_HID), 0);
  EXPECT_LT(H5O__num_attrs_test(H5I_INVALID_HID, &nattrs), 0);
  EXPECT_LT(H5O__attr_dense_info_test(H5I_INVALID_HID, &name_cnt, &corder_cnt),
            0);
  EXPECT_LT(H5O__check_msg_marked_test(H5I_INVALID_HID, false), 0);
  EXPECT_LT(H5O__msg_get_chunkno_test(H5I_INVALID_HID, H5O_NAME_ID, &chunkno),
            0);
  EXPECT_LT(H5O__msg_move_to_new_chunk_test(H5I_INVALID_HID, H5O_NAME_ID), 0);

  // Valid object whose loc->addr is temporarily set to HADDR_UNDEF ->
  // H5O_protect fails cleanly for all protect callers.
  ScopedFile file = CreateInMemoryFile("invalid_addr.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gcreate2(file.get(), kGroupName, H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  H5O_loc_t* loc = H5O_get_loc(grp.get());
  ASSERT_NE(loc, nullptr);
  const haddr_t saved_addr = loc->addr;
  loc->addr = HADDR_UNDEF;
  absl::Cleanup restore_addr = [loc, saved_addr] { loc->addr = saved_addr; };

  EXPECT_LT(H5O__is_attr_dense_test(grp.get()), 0);
  EXPECT_LT(H5O__is_attr_empty_test(grp.get()), 0);
  EXPECT_LT(H5O__num_attrs_test(grp.get(), &nattrs), 0);
  EXPECT_LT(H5O__attr_dense_info_test(grp.get(), &name_cnt, &corder_cnt), 0);
  EXPECT_LT(H5O__msg_get_chunkno_test(grp.get(), H5O_LINFO_ID, &chunkno), 0);
  EXPECT_LT(H5O__msg_move_to_new_chunk_test(grp.get(), H5O_LINFO_ID), 0);

  {
    ScopedApiContext api_ctx;
    unsigned rc = 0;
    EXPECT_LT(H5O__check_msg_marked_test(grp.get(), false), 0);
    EXPECT_LT(H5O__expunge_chunks_test(loc), 0);
    EXPECT_LT(H5O__get_rc_test(loc, &rc), 0);
  }
}

TEST(H5OtestTest, CheckMsgMarkedTestVerifiesUnknownFlagTransitions) {
  // 1. Normal object without an unknown message fails with NOTFOUND.
  {
    ScopedFile file = CreateInMemoryFile("no_unknown.h5");
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(H5Gcreate2(file.get(), kGroupName, H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    ScopedApiContext api_ctx;
    ScopedErrorSilence silence;
    EXPECT_LT(H5O__check_msg_marked_test(grp.get(), false), 0);
  }

  // 2. Build an in-memory v2 file with a middle `H5O_NULL_ID` slot and patch it
  // into an unknown message with `H5O_MSG_FLAG_MARK_IF_UNKNOWN`.
  std::vector<uint8_t> image;
  haddr_t oh_addr = HADDR_UNDEF;
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_V18), 0);
    ScopedFile file(H5Fcreate("marked_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(H5Gcreate2(file.get(), kGroupName, H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    ASSERT_TRUE(AddInt32Attr(grp.get(), kFirstAttrName, 1));
    ASSERT_TRUE(AddInt32Attr(grp.get(), kSecondAttrName, 2));
    ASSERT_GE(H5Adelete(grp.get(), kFirstAttrName), 0);

    H5O_loc_t* loc = H5O_get_loc(grp.get());
    ASSERT_NE(loc, nullptr);
    oh_addr = loc->addr;
    grp.reset();

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t sz = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(sz, 0);
    image.resize(static_cast<size_t>(sz));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()), sz);
  }
  ASSERT_TRUE(
      PatchNullMessageToUnknown(&image, oh_addr, H5O_MSG_FLAG_MARK_IF_UNKNOWN));

  // 3. Reopen RDONLY: WAS_UNKNOWN must NOT be set (`flag_val == false`).
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedFile file(H5Fopen("marked_ro.h5", H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(H5Gopen2(file.get(), kGroupName, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    ScopedApiContext api_ctx;
    EXPECT_GE(H5O__check_msg_marked_test(grp.get(), false), 0);
    ScopedErrorSilence silence;
    EXPECT_LT(H5O__check_msg_marked_test(grp.get(), true), 0);
  }

  // 4. Reopen RDWR: WAS_UNKNOWN IS set (`flag_val == true`).
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedFile file(H5Fopen("marked_rw.h5", H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(H5Gopen2(file.get(), kGroupName, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    ScopedApiContext api_ctx;
    EXPECT_GE(H5O__check_msg_marked_test(grp.get(), true), 0);
    ScopedErrorSilence silence;
    EXPECT_LT(H5O__check_msg_marked_test(grp.get(), false), 0);
  }
}

TEST(H5OtestTest, GetRcAndExpungeChunksTrackLinksAndCacheEviction) {
  std::vector<uint8_t> ro_image;
  {
    ScopedFile file = CreateInMemoryFile("rc_expunge.h5");
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(H5Gcreate2(file.get(), kGroupName, H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    H5O_loc_t* loc = H5O_get_loc(grp.get());
    ASSERT_NE(loc, nullptr);

    {
      ScopedApiContext api_ctx;
      unsigned rc = 0;
      ASSERT_GE(H5O__get_rc_test(loc, &rc), 0);
      EXPECT_EQ(rc, 1U);

      ASSERT_GE(H5Lcreate_hard(file.get(), kGroupName, file.get(), "/alias",
                               H5P_DEFAULT, H5P_DEFAULT),
                0);
      ASSERT_GE(H5O__get_rc_test(loc, &rc), 0);
      EXPECT_EQ(rc, 2U);

      ASSERT_GE(H5Ldelete(file.get(), "/alias", H5P_DEFAULT), 0);
      ASSERT_GE(H5O__get_rc_test(loc, &rc), 0);
      EXPECT_EQ(rc, 1U);
    }

    // Expunge single-chunk object header cleanly after flushing dirty entries.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    {
      ScopedApiContext api_ctx;
      EXPECT_GE(H5O__expunge_chunks_test(loc), 0);
    }

    grp.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t sz = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(sz, 0);
    ro_image.resize(static_cast<size_t>(sz));
    ASSERT_EQ(H5Fget_file_image(file.get(), ro_image.data(), ro_image.size()),
              sz);
  }

  // Opening RDONLY must make H5O__expunge_chunks_test and
  // H5O__msg_move_to_new_chunk_test fail cleanly in H5O_protect (write intent).
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), ro_image.data(), ro_image.size()), 0);
  ScopedFile ro_file(H5Fopen("rc_ro.h5", H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(ro_file.valid());
  ScopedGroup ro_grp(H5Gopen2(ro_file.get(), kGroupName, H5P_DEFAULT));
  ASSERT_TRUE(ro_grp.valid());

  ScopedErrorSilence silence;
  {
    ScopedApiContext api_ctx;
    EXPECT_LT(H5O__expunge_chunks_test(H5O_get_loc(ro_grp.get())), 0);
  }
  EXPECT_LT(H5O__msg_move_to_new_chunk_test(ro_grp.get(), H5O_LINFO_ID), 0);
}

TEST(H5OtestTest, MsgGetChunknoAndMoveToNewChunkCoverAllChunkLayoutBranches) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);

  ScopedFile file(
      H5Fcreate("move_chunks.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  // 1. Missing message type: H5O__msg_get_chunkno_test fails with NOTFOUND and
  // H5O__msg_move_to_new_chunk_test returns cleanly as a no-op.
  {
    ScopedGroup grp(H5Gcreate2(file.get(), "/noop_grp", H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    unsigned chunkno = 999;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5O__msg_get_chunkno_test(grp.get(), H5O_NAME_ID, &chunkno), 0);
    }
    EXPECT_GE(H5O__msg_move_to_new_chunk_test(grp.get(), H5O_NAME_ID), 0);
  }

  // 2. Non-last message with NO following NULL message (`null_size == 0`
  // loop runs through all non-null messages): 3D contiguous dataset has a
  // sufficiently large (>= 16 byte) `H5O_SDSPACE_ID` in chunk 0 followed by
  // `H5O_DTYPE_ID`.
  {
    const std::array<hsize_t, 3> dims = {2, 3, 4};
    ScopedSpace space(H5Screate_simple(3, dims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), kDatasetName, H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    unsigned chunkno = 999;
    ASSERT_GE(H5O__msg_get_chunkno_test(dset.get(), H5O_SDSPACE_ID, &chunkno),
              0);
    EXPECT_EQ(chunkno, 0U);

    ASSERT_GE(H5O__msg_move_to_new_chunk_test(dset.get(), H5O_SDSPACE_ID), 0);
    ASSERT_GE(H5O__msg_get_chunkno_test(dset.get(), H5O_SDSPACE_ID, &chunkno),
              0);
    EXPECT_GT(chunkno, 0U);

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ScopedApiContext api_ctx;
    EXPECT_GE(H5O__expunge_chunks_test(H5O_get_loc(dset.get())), 0);
  }

  // 3. Non-last message immediately followed by a NULL message in the same
  // chunk (`tmp_msg->type->id == H5O_NULL_ID` match branch).
  {
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_est_link_info(gcpl.get(), 12, 16), 0);

    ScopedGroup grp(H5Gcreate2(file.get(), "/null_after_grp", H5P_DEFAULT,
                               gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    // Place H5O_NAME_ID (>= 16 bytes) in chunk 0's reserved link space.
    ASSERT_GE(H5Oset_comment(grp.get(), "comment_payload_20ch"), 0);

    // Append two `H5O_MTIME_NEW_ID` messages directly into `oh` right after
    // `H5O_NAME_ID`, then release the first in place via `H5O__release_mesg`
    // without triggering header condensation, leaving `[NAME | NULL | MTIME]`.
    bool followed_by_null = false;
    {
      ScopedApiContext api_ctx;
      const H5O_loc_t* loc = H5O_get_loc(grp.get());
      H5O_t* oh = H5O_protect(loc, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      absl::Cleanup unprotect_oh = [&] {
        H5O_unprotect(loc, oh, H5AC__DIRTIED_FLAG);
      };

      time_t ts = 1700000000;
      ASSERT_GE(
          H5O_msg_append_oh(loc->file, oh, H5O_MTIME_NEW_ID, 0, 0, &ts), 0);
      ASSERT_GE(
          H5O_msg_append_oh(loc->file, oh, H5O_MTIME_NEW_ID, 0, 0, &ts), 0);

      // Locate `H5O_NAME_ID` and release the message immediately following it.
      uint8_t* end_name = nullptr;
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NAME_ID) {
          end_name = oh->mesg[i].raw + oh->mesg[i].raw_size;
          break;
        }
      }
      ASSERT_NE(end_name, nullptr);
      for (size_t j = 0; j < oh->nmesgs; ++j) {
        if (oh->mesg[j].type->id == H5O_MTIME_NEW_ID &&
            (oh->mesg[j].raw - H5O_SIZEOF_MSGHDR_OH(oh)) == end_name) {
          ASSERT_GE(H5O__release_mesg(loc->file, oh, &oh->mesg[j], false), 0);
          followed_by_null = true;
          break;
        }
      }
    }
    EXPECT_TRUE(followed_by_null);

    unsigned chunkno = 999;
    ASSERT_GE(H5O__msg_get_chunkno_test(grp.get(), H5O_NAME_ID, &chunkno), 0);
    EXPECT_EQ(chunkno, 0U);

    ASSERT_GE(H5O__msg_move_to_new_chunk_test(grp.get(), H5O_NAME_ID), 0);
    ASSERT_GE(H5O__msg_get_chunkno_test(grp.get(), H5O_NAME_ID, &chunkno), 0);
    EXPECT_GT(chunkno, 0U);

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ScopedApiContext api_ctx;
    EXPECT_GE(H5O__expunge_chunks_test(H5O_get_loc(grp.get())), 0);
  }

  // 4. Last message in chunk with trailing `gap > 0` (`end_msg ==
  // end_chunk_data` branch).
  {
    ScopedGroup grp(H5Gcreate2(file.get(), "/gap_end_grp", H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    // Put a huge attr in chunk 1 (~350B) and an anchor attr (> chunk 0 free
    // space) in chunk 2, then delete `big` so chunk 1 has a large trailing
    // NULL message.
    ASSERT_TRUE(AddLargeInt64ArrayAttr(grp.get(), "big", 40));
    ASSERT_TRUE(AddLargeInt64ArrayAttr(grp.get(), "anchor", 20));
    ASSERT_GE(H5Adelete(grp.get(), "big"), 0);

    size_t trailing_null_size = 0;
    {
      ScopedApiContext api_ctx;
      const H5O_loc_t* loc = H5O_get_loc(grp.get());
      H5O_t* oh = H5O_protect(loc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].chunkno == 1 && oh->mesg[i].type->id == H5O_NULL_ID) {
          trailing_null_size = oh->mesg[i].raw_size;
        }
      }
      ASSERT_GE(H5O_unprotect(loc, oh, H5AC__NO_FLAGS_SET), 0);
    }
    ASSERT_GE(trailing_null_size, 20U);

    // Comment raw size is `len + 1`. Setting len = trailing_null_size - 2 leaves
    // 1 byte (< message header size), forcing a 1-byte chunk gap right after
    // `H5O_NAME_ID`.
    const std::string exact_comment(trailing_null_size - 2, 'g');
    ASSERT_GE(H5Oset_comment(grp.get(), exact_comment.c_str()), 0);

    {
      ScopedApiContext api_ctx;
      const H5O_loc_t* loc = H5O_get_loc(grp.get());
      H5O_t* oh = H5O_protect(loc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      EXPECT_GT(oh->chunk[1].gap, 0U);
      ASSERT_GE(H5O_unprotect(loc, oh, H5AC__NO_FLAGS_SET), 0);
    }

    ASSERT_GE(H5O__msg_move_to_new_chunk_test(grp.get(), H5O_NAME_ID), 0);
    unsigned chunkno = 0;
    ASSERT_GE(H5O__msg_get_chunkno_test(grp.get(), H5O_NAME_ID, &chunkno), 0);
    EXPECT_GT(chunkno, 1U);

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ScopedApiContext api_ctx;
    EXPECT_GE(H5O__expunge_chunks_test(H5O_get_loc(grp.get())), 0);
  }
}

TEST(H5OtestTest, AinfoQueriesFailCleanlyOnCorruptAinfoMessage) {
  std::vector<uint8_t> image;
  haddr_t oh_addr = HADDR_UNDEF;
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_V18), 0);
    ScopedFile file(H5Fcreate("corrupt_ainfo_src.h5", H5F_ACC_TRUNC,
                              H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 1, 1), 0);

    ScopedGroup grp(H5Gcreate2(file.get(), kGroupName, H5P_DEFAULT, gcpl.get(),
                               H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    ASSERT_TRUE(AddInt32Attr(grp.get(), kFirstAttrName, 1));
    ASSERT_TRUE(AddInt32Attr(grp.get(), kSecondAttrName, 2));

    const H5O_loc_t* loc = H5O_get_loc(grp.get());
    ASSERT_NE(loc, nullptr);
    oh_addr = loc->addr;
    grp.reset();

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t sz = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(sz, 0);
    image.resize(static_cast<size_t>(sz));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()), sz);
  }

  ASSERT_TRUE(PatchAinfoVersion(&image, oh_addr, 99));

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile file(
      H5Fopen("corrupt_ainfo_ro.h5", H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gopen2(file.get(), kGroupName, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedErrorSilence silence;
  hsize_t nattrs = 0;
  hsize_t name_cnt = 0;
  hsize_t corder_cnt = 0;
  EXPECT_LT(H5O__is_attr_dense_test(grp.get()), 0);
  EXPECT_LT(H5O__is_attr_empty_test(grp.get()), 0);
  EXPECT_LT(H5O__num_attrs_test(grp.get(), &nattrs), 0);
  EXPECT_LT(H5O__attr_dense_info_test(grp.get(), &name_cnt, &corder_cnt), 0);
}

TEST(H5OtestTest, DenseAttrHelpersRejectInvalidOrCorruptBtreeAddresses) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);
  ScopedFile file(
      H5Fcreate("corrupt_bt2_addr.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  // Create a contiguous zero-filled dataset and flush it so `bad_bt2_addr`
  // points inside the valid file address space, outside metadata cache, to
  // non-"BTHD" bytes.
  haddr_t bad_bt2_addr = HADDR_UNDEF;
  {
    const hsize_t zero_count = 32;
    ScopedSpace space(H5Screate_simple(1, &zero_count, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "/dummy_raw", H5T_NATIVE_INT64,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<int64_t> zeros(zero_count, 0);
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT64, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, zeros.data()),
              0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    bad_bt2_addr = static_cast<haddr_t>(H5Dget_offset(dset.get()));
  }
  ASSERT_TRUE(H5_addr_defined(bad_bt2_addr));

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 1, 1), 0);
  ASSERT_GE(H5Pset_attr_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ScopedGroup grp(H5Gcreate2(file.get(), kGroupName, H5P_DEFAULT, gcpl.get(),
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  for (int i = 0; i < 4; ++i) {
    const std::string name = absl::StrCat("attr_", i);
    ASSERT_TRUE(AddInt32Attr(grp.get(), name.c_str(), i));
  }

  hsize_t name_cnt = 0;
  hsize_t corder_cnt = 0;
  ASSERT_GE(H5O__attr_dense_info_test(grp.get(), &name_cnt, &corder_cnt), 0);
  EXPECT_EQ(name_cnt, 4U);
  EXPECT_EQ(corder_cnt, 4U);

  const H5O_loc_t* loc = H5O_get_loc(grp.get());
  ASSERT_NE(loc, nullptr);

  H5O_ainfo_t saved_ainfo = {};
  bool found_ainfo = false;
  {
    ScopedApiContext api_ctx;
    H5O_t* oh = H5O_protect(loc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    for (size_t i = 0; i < oh->nmesgs; ++i) {
      if (oh->mesg[i].type->id == H5O_AINFO_ID &&
          oh->mesg[i].native != nullptr) {
        saved_ainfo = *static_cast<const H5O_ainfo_t*>(oh->mesg[i].native);
        found_ainfo = true;
        break;
      }
    }
    ASSERT_GE(H5O_unprotect(loc, oh, H5AC__NO_FLAGS_SET), 0);
  }
  ASSERT_TRUE(found_ainfo);
  ASSERT_TRUE(H5_addr_defined(saved_ainfo.fheap_addr));
  ASSERT_TRUE(H5_addr_defined(saved_ainfo.name_bt2_addr));
  ASSERT_TRUE(H5_addr_defined(saved_ainfo.corder_bt2_addr));

  absl::Cleanup restore_ainfo = [loc, saved_ainfo] {
    MutateCachedAinfo(loc, saved_ainfo.nattrs, saved_ainfo.name_bt2_addr,
                      saved_ainfo.corder_bt2_addr);
  };

  {
    ScopedErrorSilence silence;
    hsize_t nattrs = 0;

    // 1. Undefined name B-tree address -> H5O__attr_dense_info_test fails.
    ASSERT_TRUE(MutateCachedAinfo(loc, 4, HADDR_UNDEF,
                                  saved_ainfo.corder_bt2_addr));
    EXPECT_LT(H5O__attr_dense_info_test(grp.get(), &name_cnt, &corder_cnt), 0);

    // 2. Non-B-tree bytes at name_bt2_addr -> H5B2_open fails in all 3 helpers.
    ASSERT_TRUE(MutateCachedAinfo(loc, 4, bad_bt2_addr,
                                  saved_ainfo.corder_bt2_addr));
    EXPECT_LT(H5O__is_attr_empty_test(grp.get()), 0);
    EXPECT_LT(H5O__num_attrs_test(grp.get(), &nattrs), 0);
    EXPECT_LT(H5O__attr_dense_info_test(grp.get(), &name_cnt, &corder_cnt), 0);

    // 3. Valid name_bt2_addr + non-B-tree corder_bt2_addr -> closes bt2_name
    // and fails opening creation-order index.
    ASSERT_TRUE(
        MutateCachedAinfo(loc, 4, saved_ainfo.name_bt2_addr, bad_bt2_addr));
    EXPECT_LT(H5O__attr_dense_info_test(grp.get(), &name_cnt, &corder_cnt), 0);
  }

  ASSERT_TRUE(MutateCachedAinfo(loc, saved_ainfo.nattrs,
                                saved_ainfo.name_bt2_addr,
                                saved_ainfo.corder_bt2_addr));
  EXPECT_GE(H5O__attr_dense_info_test(grp.get(), &name_cnt, &corder_cnt), 0);
  EXPECT_EQ(name_cnt, 4U);
  EXPECT_EQ(corder_cnt, 4U);
}

TEST(H5OtestTest, MoveToNewChunkFailsCleanlyWhenChunkAllocFails) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(
      H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_NONE, false, 1),
      0);

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);

  ScopedFile file(H5Fcreate("move_alloc_fail.h5", H5F_ACC_TRUNC, fcpl.get(),
                            fapl.get()));
  ASSERT_TRUE(file.valid());

  const std::array<hsize_t, 3> dims = {2, 3, 4};
  ScopedSpace space(H5Screate_simple(3, dims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), kDatasetName, H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  H5O_loc_t* loc = H5O_get_loc(dset.get());
  ASSERT_NE(loc, nullptr);
  ASSERT_NE(loc->file, nullptr);
  ASSERT_NE(loc->file->shared, nullptr);

  const haddr_t saved_tmp_addr = loc->file->shared->tmp_addr;
  loc->file->shared->tmp_addr = 1;
  {
    absl::Cleanup restore_tmp_addr = [loc, saved_tmp_addr] {
      loc->file->shared->tmp_addr = saved_tmp_addr;
    };
    ScopedErrorSilence silence;
    EXPECT_LT(H5O__msg_move_to_new_chunk_test(dset.get(), H5O_SDSPACE_ID), 0);
  }

  unsigned chunkno = 0;
  ASSERT_GE(H5O__msg_move_to_new_chunk_test(dset.get(), H5O_SDSPACE_ID), 0);
  ASSERT_GE(H5O__msg_get_chunkno_test(dset.get(), H5O_SDSPACE_ID, &chunkno), 0);
  EXPECT_GT(chunkno, 0U);
}

}  // namespace
}  // namespace hdf5
