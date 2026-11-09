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
// Tests metadata cache image object header messages (H5O_MSG_MDCI): public
// cache image lifecycle (create, inspect, invalidate) and internal encode,
// decode, copy, size, delete, free, and debug routines
// (third_party/hdf5/src/H5Ocache_image.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ocache_image.c

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match its struct layout for internal headers.
#ifndef NDEBUG
#define H5O_CACHE_IMAGE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5F_FRIEND
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FDprivate.h"
// H5Fpkg.h declares `H5F__efc_open(bool try, ...)`, which collides with the
// C++ `try` keyword.
#pragma push_macro("try")
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#undef try
#define try h5fpkg_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#undef try
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5O_CACHE_IMAGE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5O_CACHE_IMAGE_TEST_RESTORE_UNDEF_NDEBUG
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

ScopedPlist MakeLatestCacheImageFapl(bool generate_image, int entry_ageout) {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  if (!fapl.valid()) return ScopedPlist();
  if (H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST) <
      0) {
    return ScopedPlist();
  }
  H5AC_cache_image_config_t config{};
  config.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
  config.generate_image = generate_image;
  config.save_resize_status = false;
  config.entry_ageout = entry_ageout;
  if (H5Pset_mdc_image_config(fapl.get(), &config) < 0) {
    return ScopedPlist();
  }
  return fapl;
}

TEST(H5OcacheImageTest, PublicCacheImageCreateInspectAndInvalidate) {
  TempFile temp_file("mdci_lifecycle.h5");

  // 1. Create a file with metadata cache image enabled and populate objects.
  {
    ScopedPlist fapl = MakeLatestCacheImageFapl(
        /*generate_image=*/true, H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE);
    ASSERT_TRUE(fapl.valid());

    ScopedFile file(H5Fcreate(temp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedGroup grp(
        H5Gcreate2(file.get(), "/group", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    const hsize_t dims[1] = {4};
    ScopedSpace space(H5Screate_simple(1, dims, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(grp.get(), "dset", H5T_NATIVE_INT,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    const int vals[4] = {1, 2, 3, 4};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, vals),
              0);
  }

  // 2. Reopen read-only to decode the MDCI message from superblock extension.
  haddr_t image_addr = HADDR_UNDEF;
  hsize_t image_size = 0;
  {
    ScopedPlist fapl = MakeLatestCacheImageFapl(
        /*generate_image=*/false, H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fopen(temp_file.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(file.valid());

    ASSERT_GE(H5Fget_mdc_image_info(file.get(), &image_addr, &image_size), 0);
    EXPECT_NE(image_addr, HADDR_UNDEF);
    EXPECT_GT(image_size, 0U);

    // Trigger metadata cache load while reading the dataset.
    ScopedDataset dset(H5Dopen2(file.get(), "/group/dset", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    int read_vals[4] = {};
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      read_vals),
              0);
    EXPECT_EQ(read_vals[3], 4);
  }

  // 3. Reopen R/W and touch metadata: loads image then removes MDCI message
  //    (calling H5O__mdci_delete with closing == false -> H5MF_xfree).
  {
    ScopedPlist fapl = MakeLatestCacheImageFapl(
        /*generate_image=*/false, H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fopen(temp_file.c_str(), H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedGroup extra(
        H5Gcreate2(file.get(), "/extra", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(extra.valid());
  }

  // 4. Confirm image was deleted after the R/W close without generate_image.
  {
    ScopedPlist fapl = MakeLatestCacheImageFapl(
        /*generate_image=*/false, H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fopen(temp_file.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(file.valid());

    haddr_t cleared_addr = 0;
    hsize_t cleared_size = 999;
    ASSERT_GE(
        H5Fget_mdc_image_info(file.get(), &cleared_addr, &cleared_size), 0);
    EXPECT_EQ(cleared_addr, HADDR_UNDEF);
    EXPECT_EQ(cleared_size, 0U);
  }
}

TEST(H5OcacheImageTest, PublicInvalidConfigAndFlushLifecycle) {
  // Non-default entry_ageout values other than
  // H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE are rejected cleanly.
  EXPECT_FALSE(MakeLatestCacheImageFapl(/*generate_image=*/true,
                                        /*entry_ageout=*/0)
                   .valid());

  TempFile temp_file("mdci_flush_close.h5");
  {
    ScopedPlist fapl = MakeLatestCacheImageFapl(
        /*generate_image=*/true, H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate(temp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
  }
  ScopedPlist fapl = MakeLatestCacheImageFapl(
      /*generate_image=*/false, H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE);
  ASSERT_TRUE(fapl.valid());
  ScopedFile reopened(H5Fopen(temp_file.c_str(), H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(reopened.valid());
}

TEST(H5OcacheImageTest, InternalEncodeDecodeCopySizeDeleteFreeAndDebug) {
  EXPECT_EQ(H5O_MSG_MDCI[0].id, static_cast<unsigned>(H5O_MDCI_MSG_ID));
  EXPECT_STREQ(H5O_MSG_MDCI[0].name, "mdci");
  EXPECT_EQ(H5O_MSG_MDCI[0].native_size, sizeof(H5O_mdci_t));

  ScopedFile file = CreateInMemoryFile("mdci_internal.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  // Allocate a real space so EOA > addr + size.
  const haddr_t alloc_addr = H5MF_alloc(f, H5FD_MEM_SUPER, 64);
  ASSERT_NE(alloc_addr, HADDR_UNDEF);

  const H5O_mdci_t src{alloc_addr, 64};
  const size_t raw_size =
      H5O_msg_raw_size(f, H5O_MDCI_MSG_ID, /*disable_shared=*/false, &src);
  EXPECT_EQ(raw_size, 1U + H5F_SIZEOF_ADDR(f) + H5F_SIZEOF_SIZE(f));

  std::array<uint8_t, 64> buf{};
  ASSERT_GE(
      H5O_msg_encode(f, H5O_MDCI_MSG_ID, /*disable_shared=*/false, buf.data(),
                     &src),
      0);

  // Decode valid buffer.
  auto* decoded = static_cast<H5O_mdci_t*>(
      H5O_msg_decode(f, nullptr, H5O_MDCI_MSG_ID, raw_size, buf.data()));
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->addr, src.addr);
  EXPECT_EQ(decoded->size, src.size);

  // Copy with dest == nullptr (allocates) and dest != nullptr (in-place).
  auto* copied_alloc =
      static_cast<H5O_mdci_t*>(H5O_msg_copy(H5O_MDCI_MSG_ID, decoded, nullptr));
  ASSERT_NE(copied_alloc, nullptr);
  EXPECT_EQ(copied_alloc->addr, src.addr);
  EXPECT_EQ(copied_alloc->size, src.size);

  H5O_mdci_t copied_inplace{};
  void* ret_ptr = H5O_msg_copy(H5O_MDCI_MSG_ID, decoded, &copied_inplace);
  EXPECT_EQ(ret_ptr, &copied_inplace);
  EXPECT_EQ(copied_inplace.addr, src.addr);
  EXPECT_EQ(copied_inplace.size, src.size);

  EXPECT_EQ(H5O_msg_free(H5O_MDCI_MSG_ID, copied_alloc), nullptr);
  EXPECT_EQ(H5O_msg_free(H5O_MDCI_MSG_ID, decoded), nullptr);

  // Debug output formatting via in-memory FILE*.
  std::array<char, 256> dbg_buf{};
  FILE* mem_stream = fmemopen(dbg_buf.data(), dbg_buf.size() - 1, "w");
  ASSERT_NE(mem_stream, nullptr);
  EXPECT_GE(H5O_debug_id(H5O_MDCI_MSG_ID, f, &src, mem_stream, 2, 42), 0);
  std::fclose(mem_stream);
  EXPECT_THAT(dbg_buf.data(),
              HasSubstr("Metadata Cache Image Block address:"));
  EXPECT_THAT(dbg_buf.data(),
              HasSubstr("Metadata Cache Image Block size in bytes:"));

  // Delete with HADDR_UNDEF (no-op).
  H5O_mdci_t undef_msg{HADDR_UNDEF, 0};
  EXPECT_GE(H5O_msg_delete(f, nullptr, H5O_MDCI_MSG_ID, &undef_msg), 0);

  // Delete with !f->shared->closing (H5MF_xfree branch).
  H5O_mdci_t freeable_msg{alloc_addr, 64};
  EXPECT_GE(H5O_msg_delete(f, nullptr, H5O_MDCI_MSG_ID, &freeable_msg), 0);

  // Delete with f->shared->closing == true (H5FD_free branch at EOF).
  haddr_t frag_addr = HADDR_UNDEF;
  hsize_t frag_size = 0;
  const haddr_t eof_block =
      H5FD_alloc(f->shared->lf, H5FD_MEM_SUPER, f, 64, &frag_addr, &frag_size);
  ASSERT_NE(eof_block, HADDR_UNDEF);
  ASSERT_EQ(frag_size, 0U);
  const bool prev_closing = f->shared->closing;
  f->shared->closing = true;
  absl::Cleanup restore_closing = [&] { f->shared->closing = prev_closing; };
  H5O_mdci_t closing_msg{eof_block, 64};
  EXPECT_GE(H5O_msg_delete(f, nullptr, H5O_MDCI_MSG_ID, &closing_msg), 0);
}

TEST(H5OcacheImageTest, DecodeRejectsMalformedInputs) {
  ScopedFile file = CreateInMemoryFile("mdci_decode_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_SUPER);
  ASSERT_NE(eoa, HADDR_UNDEF);

  std::array<uint8_t, 64> buf{};
  const H5O_mdci_t valid_msg{0, 16};
  const size_t full_size =
      H5O_msg_raw_size(f, H5O_MDCI_MSG_ID, false, &valid_msg);
  ASSERT_LE(full_size, buf.size());
  ASSERT_GE(H5O_msg_encode(f, H5O_MDCI_MSG_ID, false, buf.data(), &valid_msg),
            0);

  ScopedErrorSilence silence;

  // 0-byte buffer: use buf.data() + 1 so (p + p_size - 1) stays in-bounds.
  EXPECT_EQ(
      H5O_msg_decode(f, nullptr, H5O_MDCI_MSG_ID, /*buf_size=*/0,
                     buf.data() + 1),
      nullptr);

  // Unsupported message version.
  std::array<uint8_t, 64> bad_ver = buf;
  bad_ver[0] = 1;
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MDCI_MSG_ID, full_size,
                           bad_ver.data()),
            nullptr);

  // Truncated before address field.
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MDCI_MSG_ID, /*buf_size=*/1,
                           buf.data()),
            nullptr);

  // Truncated before size field.
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MDCI_MSG_ID,
                           /*buf_size=*/1 + H5F_sizeof_addr(f), buf.data()),
            nullptr);

  // Address + size overflows address space.
  const H5O_mdci_t overflow_msg{100, HADDR_UNDEF - 50};
  std::array<uint8_t, 64> overflow_buf{};
  ASSERT_GE(H5O_msg_encode(f, H5O_MDCI_MSG_ID, false, overflow_buf.data(),
                           &overflow_msg),
            0);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MDCI_MSG_ID, full_size,
                           overflow_buf.data()),
            nullptr);

  // Address + size exceeds file EOA.
  const H5O_mdci_t past_eoa_msg{eoa, 64};
  std::array<uint8_t, 64> past_eoa_buf{};
  ASSERT_GE(H5O_msg_encode(f, H5O_MDCI_MSG_ID, false, past_eoa_buf.data(),
                           &past_eoa_msg),
            0);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MDCI_MSG_ID, full_size,
                           past_eoa_buf.data()),
            nullptr);
}

}  // namespace
}  // namespace hdf5
