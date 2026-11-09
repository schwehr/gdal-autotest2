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
// Tests internal file I/O routines for block, selection, and vector read/write,
// tagged metadata flushing, cache eviction, and metadata checksum helpers
// (third_party/hdf5/src/H5Fio.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Fio.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FIO_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5F_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5Cprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"

#ifdef H5FIO_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FIO_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <array>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// RAII wrapper for pushing/popping an HDF5 API context required by internal
// H5PB/H5FD/H5AC helpers called from C++.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t dxpl_id = H5P_DEFAULT,
                            haddr_t tag = H5AC__SUPERBLOCK_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (dxpl_id != H5P_DEFAULT) {
        H5CX_set_dxpl(dxpl_id);
      }
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

TEST(H5FioTest, BlockAndSharedBlockReadWriteAndErrors) {
  ScopedFile file = CreateInMemoryFile("fio_block.h5");
  ASSERT_TRUE(file.valid());

  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(f, nullptr);
  H5F_shared_t* f_sh = H5F_get_shared(f);
  ASSERT_NE(f_sh, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Allocate two file blocks below EOA for H5FD_MEM_DRAW and H5FD_MEM_GHEAP.
  const haddr_t addr1 = H5F__alloc(f, H5FD_MEM_DRAW, 16, nullptr, nullptr);
  ASSERT_NE(addr1, HADDR_UNDEF);
  const haddr_t addr2 = H5F__alloc(f, H5FD_MEM_DRAW, 16, nullptr, nullptr);
  ASSERT_NE(addr2, HADDR_UNDEF);

  constexpr auto kData1 =
      std::to_array<uint8_t>({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
  constexpr auto kData2 =
      std::to_array<uint8_t>({21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36});

  // 2. Round-trip via H5F_block_write / H5F_block_read (H5FD_MEM_DRAW).
  ASSERT_GE(H5F_block_write(f, H5FD_MEM_DRAW, addr1, kData1.size(), kData1.data()), 0);
  std::array<uint8_t, 16> out1{};
  ASSERT_GE(H5F_block_read(f, H5FD_MEM_DRAW, addr1, out1.size(), out1.data()), 0);
  EXPECT_EQ(out1, kData1);

  // 3. Round-trip via H5F_shared_block_write / H5F_shared_block_read using
  // H5FD_MEM_GHEAP so the `type == H5FD_MEM_GHEAP` remapping branch executes.
  ASSERT_GE(H5F_shared_block_write(f_sh, H5FD_MEM_GHEAP, addr2, kData2.size(),
                                   kData2.data()),
            0);
  std::array<uint8_t, 16> out2{};
  ASSERT_GE(H5F_shared_block_read(f_sh, H5FD_MEM_GHEAP, addr2, out2.size(),
                                  out2.data()),
            0);
  EXPECT_EQ(out2, kData2);

  // Also cover H5FD_MEM_GHEAP branch in H5F_block_write / H5F_block_read.
  ASSERT_GE(H5F_block_write(f, H5FD_MEM_GHEAP, addr1, kData2.size(), kData2.data()), 0);
  ASSERT_GE(H5F_block_read(f, H5FD_MEM_GHEAP, addr1, out1.size(), out1.data()), 0);
  EXPECT_EQ(out1, kData2);

  // 4. Attempting I/O reaching f_sh->tmp_addr triggers BADRANGE failures without
  // overflowing unsigned haddr_t arithmetic.
  ScopedErrorSilence silence;
  const haddr_t tmp_overlap = f_sh->tmp_addr - 4;
  EXPECT_LT(H5F_shared_block_read(f_sh, H5FD_MEM_DRAW, tmp_overlap, 4, out1.data()), 0);
  EXPECT_LT(H5F_block_read(f, H5FD_MEM_DRAW, tmp_overlap, 4, out1.data()), 0);
  EXPECT_LT(H5F_shared_block_write(f_sh, H5FD_MEM_DRAW, tmp_overlap, 4, kData1.data()), 0);
  EXPECT_LT(H5F_block_write(f, H5FD_MEM_DRAW, tmp_overlap, 4, kData1.data()), 0);

  // 5. Attempting I/O beyond EOA (but < tmp_addr) fails inside H5PB / VFD.
  const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DRAW);
  ASSERT_NE(eoa, HADDR_UNDEF);
  const haddr_t past_eoa = eoa + 128;
  ASSERT_LT(past_eoa + 8, f_sh->tmp_addr);
  EXPECT_LT(H5F_shared_block_read(f_sh, H5FD_MEM_DRAW, past_eoa, 8, out1.data()), 0);
  EXPECT_LT(H5F_block_read(f, H5FD_MEM_DRAW, past_eoa, 8, out1.data()), 0);
  EXPECT_LT(H5F_shared_block_write(f_sh, H5FD_MEM_DRAW, past_eoa, 8, kData1.data()), 0);
  EXPECT_LT(H5F_block_write(f, H5FD_MEM_DRAW, past_eoa, 8, kData1.data()), 0);

  ASSERT_GE(H5F__free(f, H5FD_MEM_DRAW, addr1, 16), 0);
  ASSERT_GE(H5F__free(f, H5FD_MEM_DRAW, addr2, 16), 0);
}

TEST(H5FioTest, BlockIoWorksWithPageBufferingEnabled) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE,
                                       false, 1),
            0);
  ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 1024), 0);

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_page_buffer_size(fapl.get(), 4096, 50, 50), 0);

  ScopedFile file(H5Fcreate("fio_page_buf.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
  ASSERT_TRUE(file.valid());

  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  const haddr_t addr = H5F__alloc(f, H5FD_MEM_DRAW, 32, nullptr, nullptr);
  ASSERT_NE(addr, HADDR_UNDEF);

  std::array<uint8_t, 8> payload{10, 20, 30, 40, 50, 60, 70, 80};
  ASSERT_GE(H5F_block_write(f, H5FD_MEM_DRAW, addr, payload.size(), payload.data()), 0);
  std::array<uint8_t, 8> read_back{};
  ASSERT_GE(H5F_block_read(f, H5FD_MEM_DRAW, addr, read_back.size(), read_back.data()), 0);
  EXPECT_EQ(read_back, payload);
}

TEST(H5FioTest, SelectReadAndWriteRoundTripAndFailures) {
  ScopedFile file = CreateInMemoryFile("fio_select.h5");
  ASSERT_TRUE(file.valid());

  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(f, nullptr);
  H5F_shared_t* f_sh = H5F_get_shared(f);
  ASSERT_NE(f_sh, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Zero-count selection read/write succeeds as a no-op.
  EXPECT_GE(H5F_shared_select_write(f_sh, H5FD_MEM_DRAW, 0, nullptr, nullptr,
                                    nullptr, nullptr, nullptr),
            0);
  EXPECT_GE(H5F_shared_select_read(f_sh, H5FD_MEM_DRAW, 0, nullptr, nullptr,
                                   nullptr, nullptr, nullptr),
            0);

  // 2. Single-selection 1D int32 array round-trip for DRAW and GHEAP types.
  constexpr hsize_t kDim = 4;
  ScopedSpace mem_space(H5Screate_simple(1, &kDim, nullptr));
  ScopedSpace file_space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(mem_space.valid());
  ASSERT_TRUE(file_space.valid());

  auto* ms =
      static_cast<H5S_t*>(H5I_object_verify(mem_space.get(), H5I_DATASPACE));
  auto* fs =
      static_cast<H5S_t*>(H5I_object_verify(file_space.get(), H5I_DATASPACE));
  ASSERT_NE(ms, nullptr);
  ASSERT_NE(fs, nullptr);

  const haddr_t addr =
      H5F__alloc(f, H5FD_MEM_DRAW, kDim * sizeof(int32_t), nullptr, nullptr);
  ASSERT_NE(addr, HADDR_UNDEF);

  std::array<H5S_t*, 1> mem_spaces{ms};
  std::array<H5S_t*, 1> file_spaces{fs};
  std::array<haddr_t, 1> offsets{addr};
  std::array<size_t, 1> elem_sizes{sizeof(int32_t)};

  const std::array<int32_t, 4> write_vals{11, 22, 33, 44};
  std::array<const void*, 1> write_bufs{write_vals.data()};
  ASSERT_GE(H5F_shared_select_write(f_sh, H5FD_MEM_DRAW, 1, mem_spaces.data(),
                                    file_spaces.data(), offsets.data(),
                                    elem_sizes.data(), write_bufs.data()),
            0);

  std::array<int32_t, 4> read_vals{};
  std::array<void*, 1> read_bufs{read_vals.data()};
  ASSERT_GE(H5F_shared_select_read(f_sh, H5FD_MEM_GHEAP, 1, mem_spaces.data(),
                                   file_spaces.data(), offsets.data(),
                                   elem_sizes.data(), read_bufs.data()),
            0);
  EXPECT_THAT(read_vals, ElementsAre(11, 22, 33, 44));

  // Verify GHEAP write + DRAW read path as well.
  const std::array<int32_t, 4> write_vals2{55, 66, 77, 88};
  write_bufs[0] = write_vals2.data();
  ASSERT_GE(H5F_shared_select_write(f_sh, H5FD_MEM_GHEAP, 1, mem_spaces.data(),
                                    file_spaces.data(), offsets.data(),
                                    elem_sizes.data(), write_bufs.data()),
            0);
  read_vals.fill(0);
  ASSERT_GE(H5F_shared_select_read(f_sh, H5FD_MEM_DRAW, 1, mem_spaces.data(),
                                   file_spaces.data(), offsets.data(),
                                   elem_sizes.data(), read_bufs.data()),
            0);
  EXPECT_THAT(read_vals, ElementsAre(55, 66, 77, 88));

  // 3. Past-EOA selection read/write fails inside VFD selection layer.
  ScopedErrorSilence silence;
  offsets[0] = H5F_get_eoa(f, H5FD_MEM_DRAW) + 256;
  EXPECT_LT(H5F_shared_select_write(f_sh, H5FD_MEM_DRAW, 1, mem_spaces.data(),
                                    file_spaces.data(), offsets.data(),
                                    elem_sizes.data(), write_bufs.data()),
            0);
  EXPECT_LT(H5F_shared_select_read(f_sh, H5FD_MEM_DRAW, 1, mem_spaces.data(),
                                   file_spaces.data(), offsets.data(),
                                   elem_sizes.data(), read_bufs.data()),
            0);
}

TEST(H5FioTest, VectorReadAndWriteRoundTripAndFailures) {
  ScopedFile file = CreateInMemoryFile("fio_vector.h5");
  ASSERT_TRUE(file.valid());

  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(f, nullptr);
  H5F_shared_t* f_sh = H5F_get_shared(f);
  ASSERT_NE(f_sh, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Zero-count vector I/O succeeds cleanly.
  EXPECT_GE(H5F_shared_vector_write(f_sh, 0, nullptr, nullptr, nullptr, nullptr), 0);
  EXPECT_GE(H5F_shared_vector_read(f_sh, 0, nullptr, nullptr, nullptr, nullptr), 0);

  // 2. Multi-element vector write and read across two allocated blocks.
  const haddr_t addr1 = H5F__alloc(f, H5FD_MEM_DRAW, 8, nullptr, nullptr);
  const haddr_t addr2 = H5F__alloc(f, H5FD_MEM_DRAW, 8, nullptr, nullptr);
  ASSERT_NE(addr1, HADDR_UNDEF);
  ASSERT_NE(addr2, HADDR_UNDEF);

  std::array<H5FD_mem_t, 2> types{H5FD_MEM_DRAW, H5FD_MEM_DRAW};
  std::array<haddr_t, 2> addrs{addr1, addr2};
  std::array<size_t, 2> sizes{8, 8};

  const std::array<uint8_t, 8> w1{1, 3, 5, 7, 9, 11, 13, 15};
  const std::array<uint8_t, 8> w2{2, 4, 6, 8, 10, 12, 14, 16};
  std::array<const void*, 2> wbufs{w1.data(), w2.data()};

  ASSERT_GE(H5F_shared_vector_write(f_sh, 2, types.data(), addrs.data(),
                                    sizes.data(), wbufs.data()),
            0);

  std::array<uint8_t, 8> r1{};
  std::array<uint8_t, 8> r2{};
  std::array<void*, 2> rbufs{r1.data(), r2.data()};

  ASSERT_GE(H5F_shared_vector_read(f_sh, 2, types.data(), addrs.data(),
                                   sizes.data(), rbufs.data()),
            0);
  EXPECT_EQ(r1, w1);
  EXPECT_EQ(r2, w2);

  // 3. Past-EOA vector read/write fails inside VFD vector layer.
  ScopedErrorSilence silence;
  addrs[0] = H5F_get_eoa(f, H5FD_MEM_DRAW) + 256;
  EXPECT_LT(H5F_shared_vector_write(f_sh, 1, types.data(), addrs.data(),
                                    sizes.data(), wbufs.data()),
            0);
  EXPECT_LT(H5F_shared_vector_read(f_sh, 1, types.data(), addrs.data(),
                                   sizes.data(), rbufs.data()),
            0);
}

TEST(H5FioTest, FlushTaggedMetadataAndEvictCacheEntriesAndFailures) {
  ScopedFile file = CreateInMemoryFile("fio_cache.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/tagged_grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  H5O_info2_t oinfo{};
  ASSERT_GE(H5Oget_info3(grp.get(), &oinfo, H5O_INFO_BASIC), 0);
  haddr_t grp_tag = HADDR_UNDEF;
  ASSERT_GE(H5VLnative_token_to_addr(file.get(), oinfo.token, &grp_tag), 0);
  grp.reset();

  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx(H5P_DEFAULT, grp_tag);
  ASSERT_TRUE(ctx.valid());

  EXPECT_GE(H5F_flush_tagged_metadata(f, grp_tag), 0);
  EXPECT_GE(H5F_flush_tagged_metadata(f, H5AC__SUPERBLOCK_TAG), 0);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
  EXPECT_GE(H5F__evict_cache_entries(f), 0);

  // Enabling slist prior to H5AC_flush_tagged_metadata / H5AC_evict forces
  // H5C_set_slist_enabled(..., true, ...) to fail cleanly.
  ASSERT_GE(H5C_set_slist_enabled(f->shared->cache, true, false), 0);
  absl::Cleanup disable_slist = [f] {
    H5C_set_slist_enabled(f->shared->cache, false, false);
  };

  ScopedErrorSilence silence;
  EXPECT_LT(H5F_flush_tagged_metadata(f, H5AC__SUPERBLOCK_TAG), 0);
  EXPECT_LT(H5F__evict_cache_entries(f), 0);
}

TEST(H5FioTest, GetChecksumsDecodesValidAndRejectsUndersizedBuffers) {
  // 1. Buffer smaller than H5_SIZEOF_CHKSUM (4 bytes) returns failure.
  std::array<uint8_t, 3> tiny{1, 2, 3};
  uint32_t stored = 0;
  uint32_t computed = 0;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5F_get_checksums(tiny.data(), tiny.size(), &stored, &computed), 0);
  }

  // 2. Encode little-endian Jenkins checksum in the last 4 bytes of a buffer.
  std::array<uint8_t, 12> buf{0x10, 0x20, 0x30, 0x40, 0x55, 0x66, 0x77, 0x88};
  const uint32_t expected = H5_checksum_metadata(buf.data(), 8, 0);
  buf[8] = static_cast<uint8_t>(expected & 0xffU);
  buf[9] = static_cast<uint8_t>((expected >> 8) & 0xffU);
  buf[10] = static_cast<uint8_t>((expected >> 16) & 0xffU);
  buf[11] = static_cast<uint8_t>((expected >> 24) & 0xffU);

  ASSERT_GE(H5F_get_checksums(buf.data(), buf.size(), &stored, &computed), 0);
  EXPECT_EQ(stored, expected);
  EXPECT_EQ(computed, expected);

  // 3. Optional output pointers may individually be nullptr.
  stored = 0;
  ASSERT_GE(H5F_get_checksums(buf.data(), buf.size(), &stored, nullptr), 0);
  EXPECT_EQ(stored, expected);

  computed = 0;
  ASSERT_GE(H5F_get_checksums(buf.data(), buf.size(), nullptr, &computed), 0);
  EXPECT_EQ(computed, expected);

  EXPECT_GE(H5F_get_checksums(buf.data(), buf.size(), nullptr, nullptr), 0);
}

}  // namespace
}  // namespace hdf5
