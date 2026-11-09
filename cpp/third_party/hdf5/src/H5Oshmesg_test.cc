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
// Tests Shared Object Header Message table superblock extension messages
// (encoding, decoding, copying, sizing, debug formatting, and persistence
// across reopen) in third_party/hdf5/src/H5Oshmesg.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Oshmesg.c

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
#define H5OSHMESG_TEST_RESTORE_UNDEF_NDEBUG 1
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
#define H5O_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5OSHMESG_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OSHMESG_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

class ScopedApiContext {
 public:
  explicit ScopedApiContext(H5AC_ring_t ring = H5AC_RING_USER,
                            haddr_t tag = H5AC__IGNORE_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (ring != H5AC_RING_USER) {
        H5AC_set_ring(ring, &prev_ring_);
        ring_set_ = true;
      }
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
    if (ring_set_) {
      H5AC_set_ring(prev_ring_, nullptr);
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
  H5AC_ring_t prev_ring_ = H5AC_RING_INV;
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool ring_set_ = false;
  bool tagged_ = false;
};

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

TEST(H5OshmesgTest, EncodeDecodeSizeCopyFreeAcrossAddrSizes) {
  for (const size_t sizeof_addr : {size_t{4}, size_t{8}}) {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_sizes(fcpl.get(), sizeof_addr, sizeof_addr), 0);

    ScopedFile file = CreateInMemoryFile("shmesg_roundtrip.h5", H5F_ACC_TRUNC,
                                           fcpl.get());
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(H5F_SIZEOF_ADDR(f), sizeof_addr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    H5O_shmesg_table_t src{};
    src.version = 1;
    src.addr = sizeof_addr == 4 ? haddr_t{0x12345678ULL}
                                : haddr_t{0x0102030405060708ULL};
    src.nindexes = 3;

    const size_t raw_sz =
        H5O_msg_raw_size(f, H5O_SHMESG_ID, /*disable_shared=*/false, &src);
    EXPECT_EQ(raw_sz, 2 + sizeof_addr);

    std::vector<uint8_t> encoded(raw_sz, 0xCCu);
    ASSERT_GE(H5O_msg_encode(f, H5O_SHMESG_ID, /*disable_shared=*/false,
                             encoded.data(), &src),
              0);
    EXPECT_EQ(encoded[0], 1u);
    EXPECT_EQ(encoded[raw_sz - 1], 3u);

    auto* decoded = static_cast<H5O_shmesg_table_t*>(H5O_msg_decode(
        f, nullptr, H5O_SHMESG_ID, encoded.size(), encoded.data()));
    ASSERT_NE(decoded, nullptr);
    absl::Cleanup free_decoded = [&] {
      H5O_msg_free(H5O_SHMESG_ID, decoded);
    };

    EXPECT_EQ(decoded->version, src.version);
    EXPECT_EQ(decoded->addr, src.addr);
    EXPECT_EQ(decoded->nindexes, src.nindexes);

    // Copy in-place.
    H5O_shmesg_table_t copy_inplace{};
    void* copy_ret = H5O_msg_copy(H5O_SHMESG_ID, decoded, &copy_inplace);
    EXPECT_EQ(copy_ret, &copy_inplace);
    EXPECT_EQ(copy_inplace.version, src.version);
    EXPECT_EQ(copy_inplace.addr, src.addr);
    EXPECT_EQ(copy_inplace.nindexes, src.nindexes);

    // Copy with allocation (freed via H5O_msg_free -> H5MM_xfree since
    // H5O_MSG_SHMESG leaves free == nullptr).
    auto* copy_alloc = static_cast<H5O_shmesg_table_t*>(
        H5O_msg_copy(H5O_SHMESG_ID, decoded, nullptr));
    ASSERT_NE(copy_alloc, nullptr);
    absl::Cleanup free_copy = [&] {
      H5O_msg_free(H5O_SHMESG_ID, copy_alloc);
    };
    EXPECT_EQ(copy_alloc->version, src.version);
    EXPECT_EQ(copy_alloc->addr, src.addr);
    EXPECT_EQ(copy_alloc->nindexes, src.nindexes);
  }
}

TEST(H5OshmesgTest, DecodeRejectsTruncatedBuffersAndFreesTempStruct) {
  ScopedFile file = CreateInMemoryFile("shmesg_decode_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  const size_t addr_sz = H5F_SIZEOF_ADDR(f);
  ASSERT_GT(addr_sz, 1u);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5O_shmesg_table_t msg{};
  msg.version = 0;
  msg.addr = 0x10203040ULL;
  msg.nindexes = 2;
  const size_t full_sz =
      H5O_msg_raw_size(f, H5O_SHMESG_ID, /*disable_shared=*/false, &msg);
  std::vector<uint8_t> encoded(full_sz, 0);
  ASSERT_GE(H5O_msg_encode(f, H5O_SHMESG_ID, /*disable_shared=*/false,
                           encoded.data(), &msg),
            0);

  ScopedErrorSilence silence;

  // Empty buffer (p_size == 0) triggers version overflow.
  std::array<uint8_t, 2> dummy{0u, 0u};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_SHMESG_ID, 0, dummy.data() + 1),
            nullptr);

  // Truncated in table address field (1..addr_sz) triggers addr overflow.
  for (size_t len : {size_t{1}, addr_sz}) {
    EXPECT_EQ(
        H5O_msg_decode(f, nullptr, H5O_SHMESG_ID, len, encoded.data()),
        nullptr);
  }

  // Truncated right before nindexes byte (1 + addr_sz).
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_SHMESG_ID, 1 + addr_sz,
                           encoded.data()),
            nullptr);
}

TEST(H5OshmesgTest, DebugPrintsTableMetadataAndPersistsInSuperblockExtension) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 2), 0);
  ASSERT_GE(
      H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_DTYPE_FLAG, 4), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(
                fcpl.get(), 1, H5O_SHMESG_SDSPACE_FLAG | H5O_SHMESG_ATTR_FLAG,
                1),
            0);

  ScopedFile file =
      CreateInMemoryFile("shmesg_sbe.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());

  // Trigger SOHM creation and superblock extension write.
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "/dset", H5T_NATIVE_INT,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  dset.reset();
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  ASSERT_NE(f->shared, nullptr);
  ASSERT_NE(f->shared->sblock, nullptr);
  ASSERT_TRUE(H5_addr_defined(f->shared->sblock->ext_addr));

  {
    ScopedApiContext sbe_ctx(H5AC_RING_SBE, H5AC__SUPERBLOCK_TAG);
    ASSERT_TRUE(sbe_ctx.valid());

    // Direct H5O_debug_id test.
    H5O_shmesg_table_t sample{};
    sample.version = 0;
    sample.addr = 0x4321;
    sample.nindexes = 5;
    char* id_buf = nullptr;
    size_t id_len = 0;
    FILE* id_stream = open_memstream(&id_buf, &id_len);
    ASSERT_NE(id_stream, nullptr);
    absl::Cleanup free_id = [&] { std::free(id_buf); };
    EXPECT_GE(H5O_debug_id(H5O_SHMESG_ID, f, &sample, id_stream, 2, 32), 0);
    std::fclose(id_stream);
    const std::string id_out(id_buf != nullptr ? id_buf : "", id_len);
    EXPECT_THAT(id_out, HasSubstr("Version:"));
    EXPECT_THAT(id_out, HasSubstr("Shared message table address:"));
    EXPECT_THAT(id_out, HasSubstr("(rel)"));
    EXPECT_THAT(id_out, HasSubstr("Number of indexes:"));
    EXPECT_THAT(id_out, HasSubstr("5"));

    // End-to-end superblock extension H5O_debug dump.
    char* sbe_buf = nullptr;
    size_t sbe_len = 0;
    FILE* sbe_stream = open_memstream(&sbe_buf, &sbe_len);
    ASSERT_NE(sbe_stream, nullptr);
    absl::Cleanup free_sbe = [&] { std::free(sbe_buf); };
    EXPECT_GE(H5O_debug(f, f->shared->sblock->ext_addr, sbe_stream, 0, 32), 0);
    std::fclose(sbe_stream);
    const std::string sbe_out(sbe_buf != nullptr ? sbe_buf : "", sbe_len);
    EXPECT_THAT(sbe_out, HasSubstr("shared message table"));
    EXPECT_THAT(sbe_out, HasSubstr("Shared message table address:"));
    EXPECT_THAT(sbe_out, HasSubstr("Number of indexes:"));
  }

  // Reopen file image read-only to force decoding H5O_SHMESG_ID from the
  // superblock extension on disk via H5Fget_info2 / FCPL inspection.
  const ssize_t img_len = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(img_len, 0);
  std::vector<uint8_t> img(static_cast<size_t>(img_len));
  ASSERT_EQ(H5Fget_file_image(file.get(), img.data(), img.size()), img_len);
  file.reset();

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), img.data(), img.size()), 0);
  ScopedFile reopened(H5Fopen("shmesg_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedPlist got_fcpl(H5Fget_create_plist(reopened.get()));
  ASSERT_TRUE(got_fcpl.valid());
  unsigned nindexes = 0;
  ASSERT_GE(H5Pget_shared_mesg_nindexes(got_fcpl.get(), &nindexes), 0);
  EXPECT_EQ(nindexes, 2u);

  unsigned flags0 = 0;
  unsigned minsize0 = 0;
  ASSERT_GE(H5Pget_shared_mesg_index(got_fcpl.get(), 0, &flags0, &minsize0), 0);
  EXPECT_EQ(flags0, static_cast<unsigned>(H5O_SHMESG_DTYPE_FLAG));
  EXPECT_EQ(minsize0, 4u);

  H5F_info2_t finfo{};
  ASSERT_GE(H5Fget_info2(reopened.get(), &finfo), 0);
  EXPECT_GT(finfo.sohm.hdr_size, 0u);
}

}  // namespace
}  // namespace hdf5
