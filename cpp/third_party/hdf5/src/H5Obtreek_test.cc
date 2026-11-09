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
// Tests the v1 B-tree 'K' values superblock-extension object header message
// (`H5O_MSG_BTREEK`) via public superblock-extension round-trips and direct
// message decode/encode/copy/size/debug entry points
// (third_party/hdf5/src/H5Obtreek.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Obtreek.c

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// Match `:hdf5` NDEBUG layout when inspecting internal H5O/H5AC structs.
#ifndef NDEBUG
#define H5OBTREEK_TEST_RESTORE_UNDEF_NDEBUG 1
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
#ifdef H5OBTREEK_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OBTREEK_TEST_RESTORE_UNDEF_NDEBUG
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

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

TEST(H5ObtreekTest, SuperblockExtensionRoundTripPreservesNonDefaultBtreeK) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_sym_k(fcpl.get(), /*ik=*/24, /*lk=*/10), 0);
  ASSERT_GE(H5Pset_istore_k(fcpl.get(), /*ik=*/48), 0);

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  // Superblock v2+ is required so non-default B-tree 'K' values are stored in
  // the superblock extension via H5O_MSG_BTREEK.
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);

  ScopedFile file(H5Fcreate("btreek_roundtrip.h5", H5F_ACC_TRUNC, fcpl.get(),
                            fapl.get()));
  ASSERT_TRUE(file.valid());
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
            image_size);
  file.reset();

  ScopedPlist reopen_fapl = MakeCoreFapl();
  ASSERT_TRUE(reopen_fapl.valid());
  ASSERT_GE(H5Pset_file_image(reopen_fapl.get(), image.data(), image.size()),
            0);

  ScopedFile reopened(
      H5Fopen("btreek_reopened.h5", H5F_ACC_RDONLY, reopen_fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedPlist read_fcpl(H5Fget_create_plist(reopened.get()));
  ASSERT_TRUE(read_fcpl.valid());
  unsigned sym_ik = 0;
  unsigned sym_lk = 0;
  unsigned istore_ik = 0;
  ASSERT_GE(H5Pget_sym_k(read_fcpl.get(), &sym_ik, &sym_lk), 0);
  ASSERT_GE(H5Pget_istore_k(read_fcpl.get(), &istore_ik), 0);
  EXPECT_EQ(sym_ik, 24u);
  EXPECT_EQ(sym_lk, 10u);
  EXPECT_EQ(istore_ik, 48u);
}

TEST(H5ObtreekTest, MessageEncodeDecodeCopySizeAndDebug) {
  ScopedFile file = CreateInMemoryFile("btreek_ops.h5");
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  EXPECT_EQ(H5O_MSG_BTREEK[0].id, H5O_BTREEK_ID);
  EXPECT_EQ(H5O_msg_class_g[H5O_BTREEK_ID], H5O_MSG_BTREEK);

  H5O_btreek_t src{};
  src.btree_k[H5B_CHUNK_ID] = 123u;
  src.btree_k[H5B_SNODE_ID] = 456u;
  src.sym_leaf_k = 789u;

  const size_t raw_size =
      H5O_msg_raw_size(f, H5O_BTREEK_ID, /*disable_shared=*/false, &src);
  ASSERT_EQ(raw_size, 7u);

  std::array<uint8_t, 7> encoded{};
  ASSERT_GE(H5O_msg_encode(f, H5O_BTREEK_ID, /*disable_shared=*/false,
                           encoded.data(), &src),
            0);
  EXPECT_EQ(encoded[0], 0u);  // H5O_BTREEK_VERSION

  auto* decoded = static_cast<H5O_btreek_t*>(
      H5O_msg_decode(f, nullptr, H5O_BTREEK_ID, encoded.size(),
                     encoded.data()));
  ASSERT_NE(decoded, nullptr);
  absl::Cleanup free_decoded = [&] { H5O_msg_free(H5O_BTREEK_ID, decoded); };
  EXPECT_EQ(decoded->btree_k[H5B_CHUNK_ID], 123u);
  EXPECT_EQ(decoded->btree_k[H5B_SNODE_ID], 456u);
  EXPECT_EQ(decoded->sym_leaf_k, 789u);

  auto* copied_alloc =
      static_cast<H5O_btreek_t*>(H5O_msg_copy(H5O_BTREEK_ID, &src, nullptr));
  ASSERT_NE(copied_alloc, nullptr);
  absl::Cleanup free_copied = [&] {
    H5O_msg_free(H5O_BTREEK_ID, copied_alloc);
  };
  EXPECT_EQ(copied_alloc->btree_k[H5B_CHUNK_ID], 123u);
  EXPECT_EQ(copied_alloc->btree_k[H5B_SNODE_ID], 456u);
  EXPECT_EQ(copied_alloc->sym_leaf_k, 789u);

  H5O_btreek_t copied_inplace{};
  void* dst_ptr = H5O_msg_copy(H5O_BTREEK_ID, &src, &copied_inplace);
  EXPECT_EQ(dst_ptr, &copied_inplace);
  EXPECT_EQ(copied_inplace.btree_k[H5B_CHUNK_ID], 123u);
  EXPECT_EQ(copied_inplace.btree_k[H5B_SNODE_ID], 456u);
  EXPECT_EQ(copied_inplace.sym_leaf_k, 789u);

  std::FILE* debug_stream = std::tmpfile();
  ASSERT_NE(debug_stream, nullptr);
  absl::Cleanup close_stream = [&] { std::fclose(debug_stream); };
  ASSERT_GE(
      H5O_debug_id(H5O_BTREEK_ID, f, &src, debug_stream, /*indent=*/0,
                   /*fwidth=*/0),
      0);
  std::rewind(debug_stream);
  std::string debug_output(512, '\0');
  const size_t nread =
      std::fread(debug_output.data(), 1, debug_output.size(), debug_stream);
  debug_output.resize(nread);
  EXPECT_THAT(debug_output,
              HasSubstr("Chunked storage internal B-tree 'K' value: 123"));
  EXPECT_THAT(debug_output,
              HasSubstr("Symbol table node internal B-tree 'K' value: 456"));
  EXPECT_THAT(debug_output, HasSubstr("Symbol table node leaf 'K' value: 789"));
}

TEST(H5ObtreekTest, DecodeRejectsInvalidVersionAndTruncatedBuffers) {
  ScopedFile file = CreateInMemoryFile("btreek_err.h5");
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  H5O_btreek_t src{};
  src.btree_k[H5B_CHUNK_ID] = 10u;
  src.btree_k[H5B_SNODE_ID] = 20u;
  src.sym_leaf_k = 30u;

  // Keep a leading guard byte before `p` so `p + 0 - 1` stays within the same
  // object and avoids UBSan pointer-overflow when checking 0-byte truncation.
  std::array<uint8_t, 8> padded{};
  ASSERT_GE(H5O_msg_encode(f, H5O_BTREEK_ID, /*disable_shared=*/false,
                           padded.data() + 1, &src),
            0);
  const uint8_t* p = padded.data() + 1;

  ScopedErrorSilence silence;
  for (size_t bad_size : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) {
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_BTREEK_ID, bad_size, p), nullptr)
        << "Expected overflow for truncated size " << bad_size;
  }

  std::array<uint8_t, 7> bad_version{};
  ASSERT_GE(H5O_msg_encode(f, H5O_BTREEK_ID, /*disable_shared=*/false,
                           bad_version.data(), &src),
            0);
  bad_version[0] = 1u;  // Unknown version != H5O_BTREEK_VERSION (0).
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_BTREEK_ID, bad_version.size(),
                           bad_version.data()),
            nullptr);
}

}  // namespace
}  // namespace hdf5
