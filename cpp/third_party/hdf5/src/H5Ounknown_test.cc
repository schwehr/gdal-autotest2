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
// Tests the H5O unknown object header message class descriptor, free list
// management, and round-trip decoding/destruction of unknown messages in
// in-memory HDF5 files (third_party/hdf5/src/H5Ounknown.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ounknown.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which alters struct layouts (such as
// omitting debug counters in H5O_t / H5AC_info_t). Match those offsets around
// both `hdf5_internal_prelude.h` and internal `H5O*` headers.
#ifndef NDEBUG
#define H5OUNKNOWN_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#include "third_party/hdf5/src/H5Opkg.h"
H5FL_EXTERN(H5O_unknown_t);
}  // extern "C"
#ifdef H5OUNKNOWN_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OUNKNOWN_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

constexpr char kGroupName[] = "/target";
constexpr char kFirstAttrName[] = "a_first";
constexpr char kSecondAttrName[] = "b_second";
constexpr uint8_t kSyntheticUnknownMsgId = 0xfe;

// Builds a v2-format in-memory HDF5 file containing group `/target` with a
// padded `H5O_NULL_ID` message in chunk 0, and returns the serialized image
// plus `/target`'s object header address.
bool BuildGroupImageWithNullMessage(std::vector<uint8_t>* out_image,
                                    haddr_t* out_oh_addr) {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) return false;
  if (H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_V18) < 0) {
    return false;
  }

  ScopedFile file(H5Fcreate("unknown_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  if (!file.valid()) return false;

  ScopedGroup grp(H5Gcreate2(file.get(), kGroupName, H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  if (!grp.valid()) return false;

  ScopedSpace space(H5Screate(H5S_SCALAR));
  if (!space.valid()) return false;

  // Create two attributes then delete the first so chunk 0 retains a middle
  // `H5O_NULL_ID` message slot that is easy to patch in place.
  const int32_t v1 = 11;
  const int32_t v2 = 42;
  {
    ScopedAttr attr1(H5Acreate2(grp.get(), kFirstAttrName, H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT));
    if (!attr1.valid() || H5Awrite(attr1.get(), H5T_NATIVE_INT32, &v1) < 0) {
      return false;
    }
  }
  {
    ScopedAttr attr2(H5Acreate2(grp.get(), kSecondAttrName, H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT));
    if (!attr2.valid() || H5Awrite(attr2.get(), H5T_NATIVE_INT32, &v2) < 0) {
      return false;
    }
  }
  if (H5Adelete(grp.get(), kFirstAttrName) < 0) return false;

  H5O_loc_t* loc = H5O_get_loc(grp.get());
  if (loc == nullptr || !H5_addr_defined(loc->addr)) return false;
  *out_oh_addr = loc->addr;

  grp.reset();
  if (H5Fflush(file.get(), H5F_SCOPE_GLOBAL) < 0) return false;

  const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
  if (image_size <= 0) return false;
  out_image->resize(static_cast<size_t>(image_size));
  const ssize_t copied =
      H5Fget_file_image(file.get(), out_image->data(), out_image->size());
  return copied == image_size;
}

// Locates the first `H5O_NULL_ID` message in a v2 object header at `oh_addr`,
// rewrites its ID to `kSyntheticUnknownMsgId` with `msg_flags`, and updates the
// chunk-0 metadata checksum.
bool PatchNullMessageToUnknown(std::vector<uint8_t>* image, haddr_t oh_addr,
                               uint8_t msg_flags) {
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
    if (msg_id == H5O_NULL_ID) {
      chunk_start[cur] = kSyntheticUnknownMsgId;
      chunk_start[cur + 3] = msg_flags;
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

TEST(H5OunknownTest, MessageClassDescriptorAndRegistryMatch) {
  ASSERT_GE(H5open(), 0);

  EXPECT_EQ(H5O_msg_class_g[H5O_UNKNOWN_ID], H5O_MSG_UNKNOWN);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].id, static_cast<unsigned>(H5O_UNKNOWN_ID));
  EXPECT_STREQ(H5O_MSG_UNKNOWN[0].name, "unknown");
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].native_size, 0U);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].share_flags, 0U);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].decode, nullptr);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].encode, nullptr);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].copy, nullptr);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].raw_size, nullptr);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].reset, nullptr);
  EXPECT_NE(H5O_MSG_UNKNOWN[0].free, nullptr);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].del, nullptr);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].link, nullptr);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].set_share, nullptr);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].can_share, nullptr);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].pre_copy_file, nullptr);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].copy_file, nullptr);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].post_copy_file, nullptr);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].get_crt_index, nullptr);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].set_crt_index, nullptr);
  EXPECT_EQ(H5O_MSG_UNKNOWN[0].debug, nullptr);
}

TEST(H5OunknownTest, AllocatesAndFreesViaFreeListAndMsgFree) {
  ASSERT_GE(H5open(), 0);

  // 1. Allocate an unknown message struct on H5O_unknown_t's free list and
  // free it through H5O_msg_free(H5O_UNKNOWN_ID, ...).
  H5O_unknown_t* first = H5FL_MALLOC(H5O_unknown_t);
  ASSERT_NE(first, nullptr);
  absl::Cleanup cleanup_first = [&first] {
    if (first != nullptr) {
      H5FL_FREE(H5O_unknown_t, first);
    }
  };
  *first = 0x42U;
  EXPECT_EQ(H5O_msg_free(H5O_UNKNOWN_ID, first), nullptr);
  first = nullptr;

  // 2. Allocate a second struct and invoke H5O__unknown_free directly via
  // H5O_msg_free_real(H5O_MSG_UNKNOWN, ...).
  H5O_unknown_t* second = H5FL_MALLOC(H5O_unknown_t);
  ASSERT_NE(second, nullptr);
  absl::Cleanup cleanup_second = [&second] {
    if (second != nullptr) {
      H5FL_FREE(H5O_unknown_t, second);
    }
  };
  *second = kSyntheticUnknownMsgId;
  EXPECT_EQ(H5O_msg_free_real(H5O_MSG_UNKNOWN, second), nullptr);
  second = nullptr;
}

TEST(H5OunknownTest, DecodesAndFreesUnknownMessageFromInMemoryFile) {
  std::vector<uint8_t> base_image;
  haddr_t oh_addr = HADDR_UNDEF;
  ASSERT_TRUE(BuildGroupImageWithNullMessage(&base_image, &oh_addr));

  // 1. Reopen read-only and read-write when an unknown message carries
  // H5O_MSG_FLAG_MARK_IF_UNKNOWN; closing the object frees the native
  // H5O_unknown_t instance through H5O__unknown_free.
  for (unsigned access_flags :
       std::array<unsigned, 2>{H5F_ACC_RDONLY, H5F_ACC_RDWR}) {
    std::vector<uint8_t> patched = base_image;
    ASSERT_TRUE(
        PatchNullMessageToUnknown(&patched, oh_addr, H5O_MSG_FLAG_MARK_IF_UNKNOWN));

    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), patched.data(), patched.size()), 0);

    ScopedFile file(H5Fopen("unknown_ro_rw.h5", access_flags, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedGroup grp(H5Gopen2(file.get(), kGroupName, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    ScopedAttr attr(H5Aopen(grp.get(), kSecondAttrName, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    int32_t read_val = 0;
    ASSERT_GE(H5Aread(attr.get(), H5T_NATIVE_INT32, &read_val), 0);
    EXPECT_EQ(read_val, 42);
  }

  // 2. Verify H5O_MSG_FLAG_FAIL_IF_UNKNOWN_ALWAYS rejects header decoding
  // cleanly without leaking the already-allocated H5O_unknown_t entry.
  std::vector<uint8_t> reject_image = base_image;
  ASSERT_TRUE(PatchNullMessageToUnknown(&reject_image, oh_addr,
                                        H5O_MSG_FLAG_FAIL_IF_UNKNOWN_ALWAYS));

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_file_image(fapl.get(), reject_image.data(), reject_image.size()),
      0);

  ScopedFile file(H5Fopen("unknown_fail.h5", H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedErrorSilence silence;
  ScopedGroup rejected(H5Gopen2(file.get(), kGroupName, H5P_DEFAULT));
  EXPECT_EQ(rejected.get(), H5I_INVALID_HID);
}

}  // namespace
}  // namespace hdf5
