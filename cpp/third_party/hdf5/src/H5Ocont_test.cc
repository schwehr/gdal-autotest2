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
// Tests object header continuation messages (H5O_MSG_CONT): multi-chunk
// compact attribute lifecycle (flush, reopen decode, attribute/object deletion)
// and internal encode, decode, raw_size, free, delete, and debug callbacks
// (third_party/hdf5/src/H5Ocont.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ocont.c

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match its struct layout for internal headers.
#ifndef NDEBUG
#define H5O_CONT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5O_CONT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5O_CONT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = 0x10000) {
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
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

void AddCompactIntAttributes(hid_t obj_id, int start_idx, int count) {
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  for (int i = 0; i < count; ++i) {
    const int idx = start_idx + i;
    const std::string name = absl::StrCat("compact_attr_", idx);
    ScopedAttr attr(H5Acreate2(obj_id, name.c_str(), H5T_NATIVE_INT,
                               scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT, &idx), 0);
  }
}

TEST(H5OcontTest, MultiChunkCompactAttributesEncodeDecodeAndDelete) {
  TempFile temp_file("h5ocont_multichunk.h5");
  constexpr int kNumAttrs = 45;

  // Create a group with large compact attribute limits so attributes spill into
  // multiple continuation chunks, then close to flush and encode H5O_MSG_CONT.
  {
    ScopedFile file(H5Fcreate(temp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 200, 190), 0);

    ScopedGroup grp(H5Gcreate2(file.get(), "/multi_chunk_grp", H5P_DEFAULT,
                               gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    // Fence chunk 0 from EOF so compact attributes allocate continuation chunks.
    ScopedGroup blocker(H5Gcreate2(file.get(), "/blocker_1", H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(blocker.valid());
    AddCompactIntAttributes(grp.get(), 0, kNumAttrs);

    H5O_native_info_t ninfo{};
    ASSERT_GE(H5Oget_native_info(grp.get(), &ninfo, H5O_NATIVE_INFO_HDR), 0);
    EXPECT_GE(ninfo.hdr.nchunks, 2U);
  }

  // Reopen to decode continuation messages on disk, condense by deleting
  // attributes, then remove the remaining object header entirely.
  {
    ScopedFile file(H5Fopen(temp_file.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    {
      ScopedGroup grp(H5Gopen2(file.get(), "/multi_chunk_grp", H5P_DEFAULT));
      ASSERT_TRUE(grp.valid());

      H5O_native_info_t before{};
      ASSERT_GE(H5Oget_native_info(grp.get(), &before, H5O_NATIVE_INFO_HDR), 0);
      EXPECT_GE(before.hdr.nchunks, 2U);

      // Verify attribute values read cleanly across continuation chunks.
      int val = -1;
      ScopedAttr last_attr(H5Aopen(grp.get(), "compact_attr_44", H5P_DEFAULT));
      ASSERT_TRUE(last_attr.valid());
      ASSERT_GE(H5Aread(last_attr.get(), H5T_NATIVE_INT, &val), 0);
      EXPECT_EQ(val, 44);
      last_attr.reset();

      for (int i = kNumAttrs - 1; i >= 0; --i) {
        const std::string name = absl::StrCat("compact_attr_", i);
        ASSERT_GE(H5Adelete(grp.get(), name.c_str()), 0);
      }
      H5O_native_info_t after{};
      ASSERT_GE(H5Oget_native_info(grp.get(), &after, H5O_NATIVE_INFO_HDR), 0);
      EXPECT_LT(after.hdr.nchunks, before.hdr.nchunks);
    }

    // Add a second multi-chunk group and delete it completely so H5O_delete
    // triggers H5O__cont_delete for every continuation chunk.
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 200, 190), 0);
    {
      ScopedGroup victim(H5Gcreate2(file.get(), "/victim_grp", H5P_DEFAULT,
                                    gcpl.get(), H5P_DEFAULT));
      ASSERT_TRUE(victim.valid());
      ScopedGroup blocker2(H5Gcreate2(file.get(), "/blocker_2", H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(blocker2.valid());
      AddCompactIntAttributes(victim.get(), 0, kNumAttrs);
    }
    ASSERT_GE(H5Ldelete(file.get(), "/victim_grp", H5P_DEFAULT), 0);
  }
}

TEST(H5OcontTest, InternalEncodeDecodeSizeFreeAndDebug) {
  EXPECT_EQ(H5O_MSG_CONT[0].id, static_cast<unsigned>(H5O_CONT_ID));
  EXPECT_STREQ(H5O_MSG_CONT[0].name, "hdr continuation");
  EXPECT_EQ(H5O_MSG_CONT[0].native_size, sizeof(H5O_cont_t));

  ScopedFile file = CreateInMemoryFile("cont_internal.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5O_cont_t cont{0x1200, 256, 2};
  const size_t raw_size =
      H5O_msg_raw_size(f, H5O_CONT_ID, /*disable_shared=*/false, &cont);
  EXPECT_EQ(raw_size, H5F_SIZEOF_ADDR(f) + H5F_SIZEOF_SIZE(f));

  std::array<uint8_t, 64> buf{};
  ASSERT_GE(
      H5O_msg_encode(f, H5O_CONT_ID, /*disable_shared=*/false, buf.data(),
                     &cont),
      0);

  auto* decoded = static_cast<H5O_cont_t*>(
      H5O_msg_decode(f, nullptr, H5O_CONT_ID, raw_size, buf.data()));
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->addr, cont.addr);
  EXPECT_EQ(decoded->size, cont.size);
  EXPECT_EQ(decoded->chunkno, 0U);
  EXPECT_EQ(H5O_msg_free(H5O_CONT_ID, decoded), nullptr);

  // Format debug output into an in-memory buffer.
  std::array<char, 256> dbg_buf{};
  FILE* mem_stream = fmemopen(dbg_buf.data(), dbg_buf.size() - 1, "w");
  ASSERT_NE(mem_stream, nullptr);
  EXPECT_GE(H5O_debug_id(H5O_CONT_ID, f, &cont, mem_stream, 2, 36), 0);
  std::fclose(mem_stream);
  EXPECT_THAT(dbg_buf.data(), HasSubstr("Continuation address:"));
  EXPECT_THAT(dbg_buf.data(), HasSubstr("Continuation size in bytes:"));
  EXPECT_THAT(dbg_buf.data(), HasSubstr("Points to chunk number:"));

  // Exercise error paths in H5O__cont_decode.
  ScopedErrorSilence silence;

  // Overflow before address field (use buf.data() + 1 so p_end stays valid).
  EXPECT_EQ(
      H5O_msg_decode(f, nullptr, H5O_CONT_ID, /*buf_size=*/0, buf.data() + 1),
      nullptr);

  // Overflow before size field.
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_CONT_ID, H5F_sizeof_addr(f),
                           buf.data()),
            nullptr);

  // Encoded size == 0 is invalid (write zeros into the size portion).
  std::array<uint8_t, 64> zero_size_buf = buf;
  std::memset(zero_size_buf.data() + H5F_sizeof_addr(f), 0, H5F_sizeof_size(f));
  EXPECT_EQ(
      H5O_msg_decode(f, nullptr, H5O_CONT_ID, raw_size, zero_size_buf.data()),
      nullptr);
}

TEST(H5OcontTest, DeleteWithInvalidChunkFailsGracefully) {
  ScopedFile file = CreateInMemoryFile("cont_delete_error.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 200, 190), 0);
  ScopedGroup grp(H5Gcreate2(file.get(), "/multi", H5P_DEFAULT, gcpl.get(),
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedGroup blocker(H5Gcreate2(file.get(), "/blocker", H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(blocker.valid());
  AddCompactIntAttributes(grp.get(), 0, 25);

  H5O_loc_t* oloc = H5O_get_loc(grp.get());
  ASSERT_NE(oloc, nullptr);

  ScopedApiContext api_ctx(oloc->addr);
  ASSERT_TRUE(api_ctx.valid());

  H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
  ASSERT_NE(oh, nullptr);
  absl::Cleanup unprotect_oh = [&] {
    H5O_unprotect(oloc, oh, H5AC__NO_FLAGS_SET);
  };
  ASSERT_GE(oh->nchunks, 2U);

  // Temporarily invalidate chunk[1].addr so H5O__chunk_delete inside
  // H5O__cont_delete propagates an error cleanly, then restore before unprotect.
  const haddr_t saved_addr = oh->chunk[1].addr;
  oh->chunk[1].addr = HADDR_UNDEF;
  H5O_cont_t bad_cont{saved_addr, oh->chunk[1].size, 1};
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5O_msg_delete(f, oh, H5O_CONT_ID, &bad_cont), 0);
  }
  oh->chunk[1].addr = saved_addr;
}

}  // namespace
}  // namespace hdf5
