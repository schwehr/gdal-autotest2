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
// Tests chunked dataset storage, indexing types, direct chunk I/O, piece
// mapping, selection I/O, raw data chunk cache (RDCC), edge-chunk options,
// resize/pruning, format conversion, and object copy (H5Dchunk.c).
//
// Google3 build caveats:
//   - Deflate (zlib) and SZIP (libaec) filters are enabled.
//   - Parallel MPI (H5_HAVE_PARALLEL) and H5D_CHUNK_DEBUG are disabled.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dchunk.c

#include "third_party/hdf5/src/hdf5.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields in internal
// metadata cache structs. Define NDEBUG around both the prelude and internal
// headers so internal struct offsets match the compiled library ABI.
#ifndef NDEBUG
#define H5DCHUNK_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#pragma push_macro("try")
#undef try
#define try h5b2_try_param
#define H5D_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5Pprivate.h"
#include "third_party/hdf5/src/H5Sprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5DCHUNK_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DCHUNK_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

// Pushes an HDF5 API context and metadata tag for direct internal calls.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t dxpl_id = H5P_DEFAULT,
                            haddr_t tag = 0x10000) {
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

H5D_t* UnwrapDataset(hid_t dset_id) {
  return static_cast<H5D_t*>(H5VL_object_verify(dset_id, H5I_DATASET));
}

ScopedPlist MakeFaplWithLibver(H5F_libver_t low, H5F_libver_t high,
                               size_t increment = 4096) {
  ScopedPlist fapl = MakeCoreFapl(increment);
  if (fapl.valid()) {
    if (H5Pset_libver_bounds(fapl.get(), low, high) < 0) {
      fapl.reset();
    }
  }
  return fapl;
}

ScopedPlist MakeChunkedDcpl(std::span<const hsize_t> chunk_dims) {
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  if (dcpl.valid()) {
    if (H5Pset_chunk(dcpl.get(), static_cast<int>(chunk_dims.size()),
                     chunk_dims.data()) < 0) {
      dcpl.reset();
    }
  }
  return dcpl;
}

// Configurable custom H5Z filter for testing filter failures and chunk growth.
// cd_values[0]:
//   0 -> passthrough copy
//   1 -> expand forward output to cd_values[1] bytes (zero-padded header)
//   2 -> fail on forward (compress)
//   3 -> fail on reverse (decompress)
//   4 -> lossless shrink of uniform 16-byte VL fill blocks to 24 bytes
constexpr H5Z_filter_t kCustomTestFilterId = H5Z_FILTER_RESERVED + 117;

size_t CustomChunkTestFilter(unsigned int flags, size_t cd_nelmts,
                             const unsigned int cd_values[], size_t nbytes,
                             size_t* buf_size, void** buf) {
  const unsigned mode = (cd_nelmts >= 1) ? cd_values[0] : 0;
  const bool reverse = (flags & H5Z_FLAG_REVERSE) != 0;

  if (!reverse && mode == 2) {
    return 0;
  }
  if (reverse && mode == 3) {
    return 0;
  }

  if (mode == 4) {
    // Lossless zero-byte RLE compressor: 8-byte header holds a 4-byte magic
    // value and 4-byte original size. Shrinks VL heap-ID chunks (~75% zeros)
    // so fill_buf_size < orig_chunk_size on subsequent chunks in
    // H5D__chunk_allocate.
    constexpr uint32_t kRleMagic = 0x524c4531u;
    constexpr size_t kHdrBytes = 8;
    if (!reverse) {
      std::vector<uint8_t> rle;
      rle.reserve(nbytes);
      const uint8_t* src = static_cast<const uint8_t*>(*buf);
      for (size_t i = 0; i < nbytes;) {
        if (src[i] == 0) {
          size_t run = 1;
          while (i + run < nbytes && src[i + run] == 0 && run < 255) {
            ++run;
          }
          rle.push_back(0);
          rle.push_back(static_cast<uint8_t>(run));
          i += run;
        } else {
          rle.push_back(src[i]);
          ++i;
        }
      }
      if (kHdrBytes + rle.size() >= nbytes) {
        return nbytes;
      }
      const size_t out_len = kHdrBytes + rle.size();
      void* out = H5allocate_memory(out_len, /*clear=*/false);
      if (out == nullptr) {
        return 0;
      }
      const uint32_t magic = kRleMagic;
      const uint32_t orig_len = static_cast<uint32_t>(nbytes);
      std::memcpy(out, &magic, sizeof(magic));
      std::memcpy(static_cast<uint8_t*>(out) + 4, &orig_len, sizeof(orig_len));
      std::memcpy(static_cast<uint8_t*>(out) + kHdrBytes, rle.data(),
                  rle.size());
      H5free_memory(*buf);
      *buf = out;
      *buf_size = out_len;
      return out_len;
    } else {
      if (nbytes < kHdrBytes) {
        return nbytes;
      }
      const uint8_t* src = static_cast<const uint8_t*>(*buf);
      uint32_t magic = 0;
      std::memcpy(&magic, src, sizeof(magic));
      if (magic != kRleMagic) {
        return nbytes;
      }
      uint32_t orig_len = 0;
      std::memcpy(&orig_len, src + 4, sizeof(orig_len));
      void* out = H5allocate_memory(orig_len, /*clear=*/false);
      if (out == nullptr) {
        return 0;
      }
      uint8_t* dst = static_cast<uint8_t*>(out);
      size_t out_pos = 0;
      for (size_t i = kHdrBytes; i < nbytes;) {
        if (src[i] == 0) {
          if (i + 1 >= nbytes) {
            H5free_memory(out);
            return 0;
          }
          const size_t run = src[i + 1];
          if (out_pos + run > orig_len) {
            H5free_memory(out);
            return 0;
          }
          std::memset(dst + out_pos, 0, run);
          out_pos += run;
          i += 2;
        } else {
          if (out_pos >= orig_len) {
            H5free_memory(out);
            return 0;
          }
          dst[out_pos++] = src[i++];
        }
      }
      if (out_pos != orig_len) {
        H5free_memory(out);
        return 0;
      }
      H5free_memory(*buf);
      *buf = out;
      *buf_size = orig_len;
      return orig_len;
    }
  }

  if (mode == 1 && cd_nelmts >= 2 && cd_values[1] > 4) {
    const size_t target_size = cd_values[1];
    if (!reverse) {
      void* out = H5allocate_memory(target_size, /*clear=*/true);
      if (out == nullptr) {
        return 0;
      }
      uint32_t orig_len = static_cast<uint32_t>(nbytes);
      std::memcpy(out, &orig_len, sizeof(orig_len));
      const size_t copy_bytes = std::min(nbytes, target_size - sizeof(orig_len));
      std::memcpy(static_cast<uint8_t*>(out) + sizeof(orig_len), *buf,
                  copy_bytes);
      H5free_memory(*buf);
      *buf = out;
      *buf_size = target_size;
      return target_size;
    } else {
      if (nbytes < sizeof(uint32_t)) {
        return 0;
      }
      uint32_t orig_len = 0;
      std::memcpy(&orig_len, *buf, sizeof(orig_len));
      void* out = H5allocate_memory(orig_len, /*clear=*/false);
      if (out == nullptr) {
        return 0;
      }
      std::memcpy(out, static_cast<const uint8_t*>(*buf) + sizeof(orig_len),
                  orig_len);
      H5free_memory(*buf);
      *buf = out;
      *buf_size = orig_len;
      return orig_len;
    }
  }
  return nbytes;
}

void RegisterCustomTestFilterOrFail(H5Z_filter_t filter_id) {
  H5Z_class2_t cls{};
  cls.version = H5Z_CLASS_T_VERS;
  cls.id = filter_id;
  cls.encoder_present = 1;
  cls.decoder_present = 1;
  cls.name = "hdf5_h5dchunk_test_filter";
  cls.can_apply = nullptr;
  cls.set_local = nullptr;
  cls.filter = CustomChunkTestFilter;
  ASSERT_GE(H5Zregister(&cls), 0);
}

// ---------------------------------------------------------------------------
// 1. Chunk Index Types, Construct/Init, and Query/Iteration APIs
// ---------------------------------------------------------------------------

TEST(H5DchunkTest, IndexTypesConstructReopenAndQueryApis) {
  struct IndexCase {
    H5F_libver_t low;
    H5F_libver_t high;
    std::array<hsize_t, 2> dims;
    std::array<hsize_t, 2> max_dims;
    std::array<hsize_t, 2> chunk_dims;
    H5D_alloc_time_t alloc_time;
    bool deflate;
    H5D_chunk_index_t expected_idx;
  };

  constexpr std::array<IndexCase, 7> kCases = {{
      {.low = H5F_LIBVER_EARLIEST,
       .high = H5F_LIBVER_V18,
       .dims = {4, 4},
       .max_dims = {4, 4},
       .chunk_dims = {2, 2},
       .alloc_time = H5D_ALLOC_TIME_LATE,
       .deflate = false,
       .expected_idx = H5D_CHUNK_IDX_BTREE},
      {.low = H5F_LIBVER_LATEST,
       .high = H5F_LIBVER_LATEST,
       .dims = {3, 4},
       .max_dims = {3, 4},
       .chunk_dims = {3, 4},
       .alloc_time = H5D_ALLOC_TIME_LATE,
       .deflate = true,
       .expected_idx = H5D_CHUNK_IDX_SINGLE},
      {.low = H5F_LIBVER_LATEST,
       .high = H5F_LIBVER_LATEST,
       .dims = {4, 4},
       .max_dims = {4, 4},
       .chunk_dims = {2, 2},
       .alloc_time = H5D_ALLOC_TIME_EARLY,
       .deflate = false,
       .expected_idx = H5D_CHUNK_IDX_NONE},
      {.low = H5F_LIBVER_LATEST,
       .high = H5F_LIBVER_LATEST,
       .dims = {4, 4},
       .max_dims = {4, 4},
       .chunk_dims = {2, 2},
       .alloc_time = H5D_ALLOC_TIME_LATE,
       .deflate = true,
       .expected_idx = H5D_CHUNK_IDX_FARRAY},
      {.low = H5F_LIBVER_LATEST,
       .high = H5F_LIBVER_LATEST,
       .dims = {4, 4},
       .max_dims = {H5S_UNLIMITED, 4},
       .chunk_dims = {2, 2},
       .alloc_time = H5D_ALLOC_TIME_LATE,
       .deflate = false,
       .expected_idx = H5D_CHUNK_IDX_EARRAY},
      {.low = H5F_LIBVER_LATEST,
       .high = H5F_LIBVER_LATEST,
       .dims = {4, 4},
       .max_dims = {H5S_UNLIMITED, H5S_UNLIMITED},
       .chunk_dims = {2, 2},
       .alloc_time = H5D_ALLOC_TIME_LATE,
       .deflate = false,
       .expected_idx = H5D_CHUNK_IDX_BT2},
      {.low = H5F_LIBVER_LATEST,
       .high = H5F_LIBVER_LATEST,
       .dims = {4, 4},
       .max_dims = {H5S_UNLIMITED, H5S_UNLIMITED},
       .chunk_dims = {2, 2},
       .alloc_time = H5D_ALLOC_TIME_INCR,
       .deflate = true,
       .expected_idx = H5D_CHUNK_IDX_BT2},
  }};

  for (size_t i = 0; i < kCases.size(); ++i) {
    const auto& tc = kCases[i];
    ScopedPlist fapl = MakeFaplWithLibver(tc.low, tc.high);
    ASSERT_TRUE(fapl.valid());

    // Add a userblock on case 0 to exercise base_addr accounting in chunk info
    // and chunk iteration APIs.
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    if (i == 0) {
      ASSERT_GE(H5Pset_userblock(fcpl.get(), 1024), 0);
    }

    // Use a persistent TempFile on NONE/EARRAY/BT2 isn't needed; Core VFD with
    // open+reopen via H5Dopen2 works in-memory while file remains open, or
    // we test H5Dopen2 within the same open Core VFD file after H5Dclose.
    ScopedFile file(
        H5Fcreate("index_test.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedSpace fspace(
        H5Screate_simple(2, tc.dims.data(), tc.max_dims.data()));
    ASSERT_TRUE(fspace.valid());

    ScopedPlist dcpl = MakeChunkedDcpl(tc.chunk_dims);
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_alloc_time(dcpl.get(), tc.alloc_time), 0);
    if (tc.deflate) {
      ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);
    }

    // Use DAPL cache overrides on odd indices to test dapl vs fapl RDCC paths.
    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl.valid());
    if (i % 2 == 1) {
      ASSERT_GE(H5Pset_chunk_cache(dapl.get(), 17, 16384, 0.25), 0);
    }

    ScopedDataset dset(H5Dcreate2(file.get(), "ds", H5T_NATIVE_INT32,
                                  fspace.get(), H5P_DEFAULT, dcpl.get(),
                                  dapl.get()));
    ASSERT_TRUE(dset.valid());

    H5D_chunk_index_t idx_type = H5D_CHUNK_IDX_NTYPES;
    ASSERT_GE(H5Dget_chunk_index_type(dset.get(), &idx_type), 0);
    EXPECT_EQ(idx_type, tc.expected_idx);

    // Check internal index_empty query before writing.
    {
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      bool is_empty = false;
      ASSERT_GE(H5D__chunk_index_empty(UnwrapDataset(dset.get()), &is_empty),
                0);
      if (tc.expected_idx == H5D_CHUNK_IDX_NONE) {
        EXPECT_FALSE(is_empty);
      } else {
        EXPECT_TRUE(is_empty);
      }
    }

    hsize_t num_chunks = 999;
    ASSERT_GE(H5Dget_num_chunks(dset.get(), fspace.get(), &num_chunks), 0);
    if (tc.expected_idx == H5D_CHUNK_IDX_NONE) {
      EXPECT_EQ(num_chunks, 4);
    } else {
      EXPECT_EQ(num_chunks, 0);
    }

    // Write full dataset and flush so all chunks are in the index.
    const size_t total = static_cast<size_t>(tc.dims[0] * tc.dims[1]);
    std::vector<int32_t> write_data(total);
    std::iota(write_data.begin(), write_data.end(), static_cast<int32_t>(i * 100));
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, write_data.data()),
              0);
    ASSERT_GE(H5Dflush(dset.get()), 0);

    // Close and re-open to exercise H5D__chunk_init(..., open_op=true).
    dset.reset();
    dset.reset(H5Dopen2(file.get(), "ds", dapl.get()));
    ASSERT_TRUE(dset.valid());

    {
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      bool is_empty = true;
      ASSERT_GE(H5D__chunk_index_empty(UnwrapDataset(dset.get()), &is_empty),
                0);
      EXPECT_FALSE(is_empty);
    }

    ASSERT_GE(H5Dget_num_chunks(dset.get(), fspace.get(), &num_chunks), 0);
    const hsize_t expected_chunks =
        (tc.expected_idx == H5D_CHUNK_IDX_SINGLE) ? 1 : 4;
    EXPECT_EQ(num_chunks, expected_chunks);

    // Exercise H5Dget_chunk_info with both full and NULL outputs, and OOB idx.
    std::array<hsize_t, 2> coord = {99, 99};
    unsigned filter_mask = 0xDEAD;
    haddr_t addr = HADDR_UNDEF;
    hsize_t size = 0;
    ASSERT_GE(H5Dget_chunk_info(dset.get(), fspace.get(), 0, coord.data(),
                                &filter_mask, &addr, &size),
              0);
    EXPECT_NE(addr, HADDR_UNDEF);
    EXPECT_GT(size, 0);
    if (i == 0) {
      // With a 1024-byte userblock, reported file address must exceed it.
      EXPECT_GE(addr, 1024);
    }
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Dget_chunk_info(dset.get(), fspace.get(), 0, nullptr, nullptr,
                                  nullptr, nullptr),
                0);
      // Out-of-bounds chunk index fails in the public API.
      coord = {99, 99};
      addr = 1234;
      size = 1234;
      EXPECT_LT(H5Dget_chunk_info(dset.get(), fspace.get(), 99, coord.data(),
                                  &filter_mask, &addr, &size),
                0);
    }

    // Querying only size via public API succeeds.
    hsize_t only_size = 0;
    EXPECT_GE(H5Dget_chunk_info(dset.get(), fspace.get(), 0, nullptr, nullptr,
                                nullptr, &only_size),
              0);
    EXPECT_GT(only_size, 0);

    // Internal H5D__get_chunk_info with OOB chunk index succeeds with
    // HADDR_UNDEF and size 0.
    {
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      auto* sp = static_cast<H5S_t*>(
          H5I_object_verify(fspace.get(), H5I_DATASPACE));
      ASSERT_NE(sp, nullptr);
      addr = 1234;
      size = 1234;
      EXPECT_GE(H5D__get_chunk_info(UnwrapDataset(dset.get()), sp, 99,
                                    coord.data(), &filter_mask, &addr, &size),
                0);
      EXPECT_EQ(addr, HADDR_UNDEF);
      EXPECT_EQ(size, 0);
    }

    // Exercise H5Dget_chunk_info_by_coord.
    std::array<hsize_t, 2> query_coord = {0, 0};
    haddr_t by_coord_addr = HADDR_UNDEF;
    hsize_t by_coord_size = 0;
    unsigned by_coord_fmask = 0xDEAD;
    ASSERT_GE(H5Dget_chunk_info_by_coord(dset.get(), query_coord.data(),
                                         &by_coord_fmask, &by_coord_addr,
                                         &by_coord_size),
              0);
    EXPECT_NE(by_coord_addr, HADDR_UNDEF);
    EXPECT_GT(by_coord_size, 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Dget_chunk_info_by_coord(dset.get(), query_coord.data(),
                                           nullptr, nullptr, nullptr),
                0);
    }
    by_coord_addr = HADDR_UNDEF;
    EXPECT_GE(H5Dget_chunk_info_by_coord(dset.get(), query_coord.data(),
                                         nullptr, &by_coord_addr, nullptr),
              0);
    EXPECT_NE(by_coord_addr, HADDR_UNDEF);

    // Exercise H5Dchunk_iter: full iteration, early stop, and error return.
    struct IterState {
      size_t count = 0;
      haddr_t first_addr = HADDR_UNDEF;
      size_t stop_at = 0;
      int return_val = H5_ITER_CONT;
    } state;

    auto iter_cb = [](const hsize_t* /*offset*/, unsigned /*filter_mask*/,
                      haddr_t chk_addr, hsize_t chk_size, void* op_data) -> int {
      auto* s = static_cast<IterState*>(op_data);
      if (s->count == 0) {
        s->first_addr = chk_addr;
      }
      EXPECT_NE(chk_addr, HADDR_UNDEF);
      EXPECT_GT(chk_size, 0);
      s->count++;
      if (s->stop_at > 0 && s->count >= s->stop_at) {
        return s->return_val;
      }
      return H5_ITER_CONT;
    };

    ASSERT_GE(H5Dchunk_iter(dset.get(), H5P_DEFAULT, iter_cb, &state), 0);
    EXPECT_EQ(state.count, expected_chunks);
    if (i == 0) {
      EXPECT_GE(state.first_addr, 1024);
    }

    if (expected_chunks > 1) {
      state = {.count = 0, .stop_at = 2, .return_val = H5_ITER_STOP};
      EXPECT_GE(H5Dchunk_iter(dset.get(), H5P_DEFAULT, iter_cb, &state), 0);
      EXPECT_EQ(state.count, 2);

      state = {.count = 0, .stop_at = 1, .return_val = H5_ITER_ERROR};
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5Dchunk_iter(dset.get(), H5P_DEFAULT, iter_cb, &state), 0);
      }
    }

    // Verify full data readback matches.
    std::vector<int32_t> read_data(total, -1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, read_data.data()),
              0);
    EXPECT_EQ(read_data, write_data);
  }
}

TEST(H5DchunkTest, ConstructInitAndQueryErrors) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile file = CreateInMemoryFile("errors.h5");
  ASSERT_TRUE(file.valid());

  // Chunk larger than fixed max_dims must fail.
  {
    ScopedErrorSilence silence;
    std::array<hsize_t, 2> dims = {4, 4};
    std::array<hsize_t, 2> bad_chunk = {8, 4};
    ScopedSpace space(H5Screate_simple(2, dims.data(), dims.data()));
    ScopedPlist dcpl = MakeChunkedDcpl(bad_chunk);
    ASSERT_TRUE(space.valid());
    ASSERT_TRUE(dcpl.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "too_big", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    EXPECT_EQ(dset.get(), H5I_INVALID_HID);
  }

  // Combining external storage (EFL) with chunked layout must fail.
  {
    ScopedErrorSilence silence;
    std::array<hsize_t, 1> dims = {16};
    std::array<hsize_t, 1> chunk = {4};
    ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(chunk);
    ASSERT_GE(H5Pset_external(dcpl.get(), "ext.bin", 0, 64), 0);
    ScopedDataset dset(H5Dcreate2(file.get(), "efl_chunk", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    EXPECT_EQ(dset.get(), H5I_INVALID_HID);
  }

  // Scalar dataspace with chunked DCPL must fail.
  {
    ScopedErrorSilence silence;
    std::array<hsize_t, 1> chunk = {4};
    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ScopedPlist dcpl = MakeChunkedDcpl(chunk);
    ScopedDataset dset(H5Dcreate2(file.get(), "scalar_chunk", H5T_NATIVE_INT32,
                                  scalar.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    EXPECT_EQ(dset.get(), H5I_INVALID_HID);
  }

  // Chunk rank mismatch must fail.
  {
    ScopedErrorSilence silence;
    std::array<hsize_t, 2> dims = {4, 4};
    std::array<hsize_t, 1> chunk = {4};
    ScopedSpace space(H5Screate_simple(2, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(chunk);
    ScopedDataset dset(H5Dcreate2(file.get(), "rank_mismatch", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    EXPECT_EQ(dset.get(), H5I_INVALID_HID);
  }

  // Large (>4GB) single chunk: rejected below V200, accepted with V200 when
  // alloc_time is LATE so no storage or cache buffer is allocated.
  {
    std::array<hsize_t, 3> dims = {1024, 1024, 1025};
    std::array<hsize_t, 3> chunk = {1024, 1024, 1025};
    ScopedSpace big_space(H5Screate_simple(3, dims.data(), nullptr));
    ScopedPlist big_dcpl = MakeChunkedDcpl(chunk);
    ASSERT_GE(H5Pset_alloc_time(big_dcpl.get(), H5D_ALLOC_TIME_LATE), 0);

    {
      ScopedErrorSilence silence;
      ScopedPlist fapl_v114 = MakeFaplWithLibver(H5F_LIBVER_V110, H5F_LIBVER_V114);
      ScopedFile fv4(H5Fcreate("big_v4.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                               fapl_v114.get()));
      ASSERT_TRUE(fv4.valid());
      ScopedDataset d4(H5Dcreate2(fv4.get(), "big", H5T_NATIVE_INT32,
                                  big_space.get(), H5P_DEFAULT, big_dcpl.get(),
                                  H5P_DEFAULT));
      EXPECT_EQ(d4.get(), H5I_INVALID_HID);
    }

    {
      ScopedPlist fapl_v200 = MakeFaplWithLibver(H5F_LIBVER_V200, H5F_LIBVER_V200);
      ScopedFile fv5(H5Fcreate("big_v5.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                               fapl_v200.get()));
      ASSERT_TRUE(fv5.valid());
      ScopedDataset d5(H5Dcreate2(fv5.get(), "big", H5T_NATIVE_INT32,
                                  big_space.get(), H5P_DEFAULT, big_dcpl.get(),
                                  H5P_DEFAULT));
      ASSERT_TRUE(d5.valid());
      d5.reset();
      d5.reset(H5Dopen2(fv5.get(), "big", H5P_DEFAULT));
      ASSERT_TRUE(d5.valid());
    }
  }

  // Unallocated chunk queries on a 1D dataset (exercises 1D init & NULL space
  // handling).
  {
    std::array<hsize_t, 1> dims = {12};
    std::array<hsize_t, 1> chunk = {4};
    ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(chunk);
    ScopedDataset dset(H5Dcreate2(file.get(), "d1_unalloc", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    // Query info by coord on unallocated chunk returns HADDR_UNDEF & size 0.
    hsize_t coord = 4;
    unsigned fmask = 123;
    haddr_t addr = 999;
    hsize_t size = 999;
    ASSERT_GE(
        H5Dget_chunk_info_by_coord(dset.get(), &coord, &fmask, &addr, &size),
        0);
    EXPECT_EQ(addr, HADDR_UNDEF);
    EXPECT_EQ(size, 0);

    // Misaligned or out-of-bounds coordinates return SUCCEED with
    // addr == HADDR_UNDEF and size == 0.
    hsize_t bad_align = 3;
    addr = 123;
    size = 123;
    EXPECT_GE(
        H5Dget_chunk_info_by_coord(dset.get(), &bad_align, &fmask, &addr, &size),
        0);
    EXPECT_EQ(addr, HADDR_UNDEF);
    EXPECT_EQ(size, 0);
    hsize_t oob = 16;
    addr = 123;
    size = 123;
    EXPECT_GE(H5Dget_chunk_info_by_coord(dset.get(), &oob, &fmask, &addr, &size),
              0);
    EXPECT_EQ(addr, HADDR_UNDEF);
    EXPECT_EQ(size, 0);

    // Invalid args, H5S_NONE selection, or unchunked dataset errors.
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Dget_chunk_info_by_coord(dset.get(), nullptr, &fmask, &addr, &size),
        0);
    EXPECT_LT(
        H5Dget_chunk_info_by_coord(dset.get(), &coord, nullptr, nullptr, nullptr),
        0);

    ScopedSpace none_sp(H5Screate_simple(1, dims.data(), nullptr));
    ASSERT_GE(H5Sselect_none(none_sp.get()), 0);
    hsize_t none_chunks = 99;
    EXPECT_GE(H5Dget_num_chunks(dset.get(), none_sp.get(), &none_chunks), 0);
    EXPECT_EQ(none_chunks, 0);
    ScopedSpace contig_sp(H5Screate_simple(1, dims.data(), nullptr));
    ScopedDataset contig(H5Dcreate2(file.get(), "contig", H5T_NATIVE_INT32,
                                    contig_sp.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
    ASSERT_TRUE(contig.valid());
    EXPECT_LT(H5Dget_num_chunks(contig.get(), space.get(), &size), 0);
    EXPECT_LT(
        H5Dget_chunk_info(contig.get(), space.get(), 0, &coord, &fmask, &addr, &size),
        0);
    EXPECT_LT(
        H5Dget_chunk_info_by_coord(contig.get(), &coord, &fmask, &addr, &size),
        0);
    EXPECT_LT(H5Dchunk_iter(contig.get(), H5P_DEFAULT, nullptr, nullptr), 0);
    EXPECT_LT(H5Dchunk_iter(dset.get(), H5P_DEFAULT, nullptr, nullptr), 0);
  }
}

// ---------------------------------------------------------------------------
// 2. Direct Chunk Write / Read / Storage Size
// ---------------------------------------------------------------------------

TEST(H5DchunkTest, DirectChunkWriteReadAndStorageSize) {
  ScopedPlist fapl = MakeFaplWithLibver(H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
  ASSERT_TRUE(fapl.valid());
  ScopedFile file(
      H5Fcreate("direct_chunk.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  // Dataset A: unfiltered 2x2 chunks in a 4x4 array (int32 -> 16 bytes/chunk).
  std::array<hsize_t, 2> dims = {4, 4};
  std::array<hsize_t, 2> chunk_dims = {2, 2};
  ScopedSpace space(H5Screate_simple(2, dims.data(), nullptr));
  ScopedPlist dcpl_unfilt = MakeChunkedDcpl(chunk_dims);
  ScopedDataset dset_unfilt(H5Dcreate2(file.get(), "unfilt", H5T_NATIVE_INT32,
                                       space.get(), H5P_DEFAULT,
                                       dcpl_unfilt.get(), H5P_DEFAULT));
  ASSERT_TRUE(dset_unfilt.valid());

  // Unallocated dataset storage size query succeeds cleanly; direct read on
  // unallocated storage fails.
  {
    ScopedErrorSilence silence;
    hsize_t st_size = 999;
    std::array<hsize_t, 2> off0 = {0, 0};
    EXPECT_GE(H5Dget_chunk_storage_size(dset_unfilt.get(), off0.data(), &st_size),
              0);
    uint32_t fmask = 0;
    size_t bsz = 16;
    std::array<int32_t, 4> dummy{};
    EXPECT_LT(H5Dread_chunk2(dset_unfilt.get(), H5P_DEFAULT, off0.data(), &fmask,
                             dummy.data(), &bsz),
              0);
  }

  // Write chunk {0, 0} directly before any regular write (hits !is_space_alloc).
  std::array<int32_t, 4> chunk00 = {10, 11, 12, 13};
  std::array<hsize_t, 2> off00 = {0, 0};
  ASSERT_GE(H5Dwrite_chunk(dset_unfilt.get(), H5P_DEFAULT, 0, off00.data(),
                           chunk00.size() * sizeof(int32_t), chunk00.data()),
            0);

  // Query storage size on an unfiltered allocated chunk (returns layout chunk
  // size directly without index lookup size override).
  hsize_t unfilt_st_size = 0;
  ASSERT_GE(
      H5Dget_chunk_storage_size(dset_unfilt.get(), off00.data(), &unfilt_st_size),
      0);
  EXPECT_EQ(unfilt_st_size, 16);

  // Also test full-chunk write with differing src vs dst type size
  // (int64_t -> int32_t) under cacheable Selection I/O OFF.
  {
    std::array<hsize_t, 2> hs20 = {2, 0};
    std::array<hsize_t, 2> cnt22 = {2, 2};
    ScopedSpace m22(H5Screate_simple(2, cnt22.data(), nullptr));
    ScopedPlist dxpl_tconv(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_GE(H5Pset_selection_io(dxpl_tconv.get(), H5D_SELECTION_IO_MODE_OFF),
              0);
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, hs20.data(),
                                  nullptr, cnt22.data(), nullptr),
              0);
    std::array<int64_t, 4> wide_vals = {100, 101, 102, 103};
    ASSERT_GE(H5Dwrite(dset_unfilt.get(), H5T_NATIVE_INT64, m22.get(),
                       space.get(), dxpl_tconv.get(), wide_vals.data()),
              0);
    ASSERT_GE(H5Sselect_all(space.get()), 0);
  }

  // Overwrite the same existing unfiltered chunk in-place directly.
  chunk00 = {20, 21, 22, 23};
  ASSERT_GE(H5Dwrite_chunk(dset_unfilt.get(), H5P_DEFAULT, 0, off00.data(),
                           chunk00.size() * sizeof(int32_t), chunk00.data()),
            0);

  // Write chunk {2, 2} via regular H5Dwrite with selection I/O OFF so it sits
  // dirty in the chunk cache, then test direct write + direct read evicting
  // cached chunks.
  ScopedPlist dxpl_cache(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(H5Pset_selection_io(dxpl_cache.get(), H5D_SELECTION_IO_MODE_OFF), 0);
  std::array<hsize_t, 2> hs_start = {2, 2};
  std::array<hsize_t, 2> hs_count = {1, 2};
  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, hs_start.data(),
                                nullptr, hs_count.data(), nullptr),
            0);
  std::array<hsize_t, 1> mdim = {2};
  ScopedSpace mspace(H5Screate_simple(1, mdim.data(), nullptr));
  std::array<int32_t, 2> partial_vals = {77, 88};
  ASSERT_GE(H5Dwrite(dset_unfilt.get(), H5T_NATIVE_INT32, mspace.get(),
                     space.get(), dxpl_cache.get(), partial_vals.data()),
            0);

  // Direct read on dirty cached chunk {2, 2} must flush/evict it first and read
  // the cached values.
  size_t query_size = 0;
  uint32_t read_filters = 0xFF;
  std::array<hsize_t, 2> off22 = {2, 2};
  ASSERT_GE(H5Dread_chunk2(dset_unfilt.get(), H5P_DEFAULT, off22.data(),
                           &read_filters, nullptr, &query_size),
            0);
  EXPECT_EQ(query_size, 16);
  EXPECT_EQ(read_filters, 0);

  // Put it back dirty in cache and call H5Dwrite_chunk on it (evicts dirty).
  partial_vals = {91, 92};
  ASSERT_GE(H5Dwrite(dset_unfilt.get(), H5T_NATIVE_INT32, mspace.get(),
                     space.get(), dxpl_cache.get(), partial_vals.data()),
            0);
  std::array<int32_t, 4> chunk22 = {30, 31, 32, 33};
  ASSERT_GE(H5Dwrite_chunk(dset_unfilt.get(), H5P_DEFAULT, 0, off22.data(),
                           chunk22.size() * sizeof(int32_t), chunk22.data()),
            0);

  // Test H5Dread_chunk2: too-small buffer skips read, returns required size &
  // filter mask, and leaves buffer untouched.
  {
    size_t small_sz = 4;
    uint32_t fmask = 0x1234;
    std::array<uint8_t, 4> small_buf{};
    EXPECT_GE(H5Dread_chunk2(dset_unfilt.get(), H5P_DEFAULT, off00.data(),
                             &fmask, small_buf.data(), &small_sz),
              0);
    EXPECT_EQ(small_sz, 16);
    EXPECT_EQ(fmask, 0);
    EXPECT_THAT(small_buf, ElementsAre(0, 0, 0, 0));

    ScopedErrorSilence silence;
    EXPECT_LT(H5Dread_chunk2(dset_unfilt.get(), H5P_DEFAULT, nullptr, &fmask,
                             small_buf.data(), &small_sz),
              0);
    EXPECT_LT(H5Dread_chunk2(dset_unfilt.get(), H5P_DEFAULT, off00.data(),
                             nullptr, small_buf.data(), &small_sz),
              0);
    EXPECT_LT(H5Dread_chunk2(dset_unfilt.get(), H5P_DEFAULT, off00.data(),
                             &fmask, small_buf.data(), nullptr),
              0);
  }

  std::array<int32_t, 4> read_chunk{};
  size_t exact_sz = sizeof(read_chunk);
  ASSERT_GE(H5Dread_chunk2(dset_unfilt.get(), H5P_DEFAULT, off00.data(),
                           &read_filters, read_chunk.data(), &exact_sz),
            0);
  EXPECT_THAT(read_chunk, ElementsAre(20, 21, 22, 23));

  // Deprecated H5Dread_chunk1 path.
  read_chunk.fill(-1);
  ASSERT_GE(H5Dread_chunk1(dset_unfilt.get(), H5P_DEFAULT, off22.data(),
                           &read_filters, read_chunk.data()),
            0);
  EXPECT_THAT(read_chunk, ElementsAre(30, 31, 32, 33));

  // Dataset B: deflate-filtered chunks with changing sizes & dirty cache query.
  ScopedPlist dcpl_filt = MakeChunkedDcpl(chunk_dims);
  ASSERT_GE(H5Pset_deflate(dcpl_filt.get(), 6), 0);
  ASSERT_GE(H5Sselect_all(space.get()), 0);
  ScopedDataset dset_filt(H5Dcreate2(file.get(), "filt", H5T_NATIVE_INT32,
                                     space.get(), H5P_DEFAULT, dcpl_filt.get(),
                                     H5P_DEFAULT));
  ASSERT_TRUE(dset_filt.valid());

  // Write partial chunk {0, 0} into cache (still dirty), then query
  // H5Dget_chunk_storage_size (must flush dirty filtered chunk to report size).
  std::array<hsize_t, 2> hs0 = {0, 0};
  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, hs0.data(), nullptr,
                                hs_count.data(), nullptr),
            0);
  ASSERT_GE(H5Dwrite(dset_filt.get(), H5T_NATIVE_INT32, mspace.get(),
                     space.get(), dxpl_cache.get(), partial_vals.data()),
            0);
  hsize_t filt_chunk_bytes = 0;
  ASSERT_GE(
      H5Dget_chunk_storage_size(dset_filt.get(), off00.data(), &filt_chunk_bytes),
      0);
  EXPECT_GT(filt_chunk_bytes, 0);

  // Overwrite with H5Dwrite_chunk passing raw bypassed chunk data (filter mask=1).
  std::array<int32_t, 4> bypass = {1, 2, 3, 4};
  ASSERT_GE(H5Dwrite_chunk(dset_filt.get(), H5P_DEFAULT, /*filters=*/1u,
                           off00.data(), sizeof(bypass), bypass.data()),
            0);
  ASSERT_GE(
      H5Dget_chunk_storage_size(dset_filt.get(), off00.data(), &filt_chunk_bytes),
      0);
  EXPECT_EQ(filt_chunk_bytes, 16);

  // Overwrite again with smaller payload (e.g. compressed size change).
  std::array<uint8_t, 6> shorter = {9, 8, 7, 6, 5, 4};
  ASSERT_GE(H5Dwrite_chunk(dset_filt.get(), H5P_DEFAULT, /*filters=*/1u,
                           off00.data(), shorter.size(), shorter.data()),
            0);
  ASSERT_GE(
      H5Dget_chunk_storage_size(dset_filt.get(), off00.data(), &filt_chunk_bytes),
      0);
  EXPECT_EQ(filt_chunk_bytes, 6);

  // Missing chunk {2, 0} direct read & storage size errors.
  {
    ScopedErrorSilence silence;
    std::array<hsize_t, 2> off20 = {2, 0};
    hsize_t sz = 0;
    EXPECT_LT(H5Dget_chunk_storage_size(dset_filt.get(), off20.data(), &sz), 0);
    size_t bsz = 0;
    uint32_t fm = 0;
    EXPECT_LT(
        H5Dread_chunk2(dset_filt.get(), H5P_DEFAULT, off20.data(), &fm, nullptr, &bsz),
        0);
    // Zero data_size, NULL buf, NULL offset, and invalid offset errors.
    EXPECT_LT(H5Dwrite_chunk(dset_unfilt.get(), H5P_DEFAULT, 0, off00.data(),
                             /*data_size=*/0, chunk00.data()),
              0);
    EXPECT_LT(H5Dwrite_chunk(dset_unfilt.get(), H5P_DEFAULT, 0, off00.data(),
                             16, nullptr),
              0);
    EXPECT_LT(H5Dwrite_chunk(dset_unfilt.get(), H5P_DEFAULT, 0, nullptr, 16,
                             chunk00.data()),
              0);
    std::array<hsize_t, 2> bad_off = {1, 0};
    EXPECT_LT(H5Dwrite_chunk(dset_unfilt.get(), H5P_DEFAULT, 0, bad_off.data(),
                             16, chunk00.data()),
              0);
    std::array<hsize_t, 2> oob_off = {8, 0};
    EXPECT_LT(H5Dwrite_chunk(dset_unfilt.get(), H5P_DEFAULT, 0, oob_off.data(),
                             16, chunk00.data()),
              0);
  }
}

// ---------------------------------------------------------------------------
// 3. Piece Mapping & Selection I/O (Single, All, Hyperslab, 1D, Points, Multi)
// ---------------------------------------------------------------------------

TEST(H5DchunkTest, PieceMappingGeometriesAndSelectionIo) {
  ScopedFile file = CreateInMemoryFile("piece_map.h5");
  ASSERT_TRUE(file.valid());

  // 3D dataset with partial edge chunks in every dimension: {5, 7, 9} with
  // chunks {2, 3, 4} -> 3 x 3 x 3 = 27 chunks.
  std::array<hsize_t, 3> dims = {5, 7, 9};
  std::array<hsize_t, 3> cdims = {2, 3, 4};
  ScopedSpace fspace(H5Screate_simple(3, dims.data(), nullptr));
  ScopedPlist dcpl = MakeChunkedDcpl(cdims);

  // Set rdcc_nbytes = 0 via DAPL so chunks are uncacheable and Selection I/O ON
  // is allowed when requested on DXPL.
  ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_GE(H5Pset_chunk_cache(dapl.get(), 0, 0, 0.0), 0);

  ScopedDataset dset(H5Dcreate2(file.get(), "cube", H5T_NATIVE_INT32,
                                fspace.get(), H5P_DEFAULT, dcpl.get(),
                                dapl.get()));
  ASSERT_TRUE(dset.valid());

  const size_t n_total = 5 * 7 * 9;
  std::vector<int32_t> expected(n_total);
  std::iota(expected.begin(), expected.end(), 1);

  ScopedPlist dxpl_sel_on(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(H5Pset_selection_io(dxpl_sel_on.get(), H5D_SELECTION_IO_MODE_ON), 0);
  ScopedPlist dxpl_sel_off(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(H5Pset_selection_io(dxpl_sel_off.get(), H5D_SELECTION_IO_MODE_OFF),
            0);

  // (A) H5S_ALL write & read over 27 chunks (>8 chunks triggers heap allocs in
  // H5D__chunk_read/write selection I/O paths) with Selection I/O ON.
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     dxpl_sel_on.get(), expected.data()),
            0);
  uint32_t actual_mode = 0;
  EXPECT_GE(H5Pget_actual_selection_io_mode(dxpl_sel_on.get(), &actual_mode), 0);
  EXPECT_NE(actual_mode, 0u);

  std::vector<int32_t> read_all(n_total, -1);
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    dxpl_sel_on.get(), read_all.data()),
            0);
  EXPECT_EQ(read_all, expected);

  // Verify H5S_ALL write + read also works with Selection I/O OFF.
  std::vector<int32_t> read_all_off(n_total, -1);
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     dxpl_sel_off.get(), expected.data()),
            0);
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    dxpl_sel_off.get(), read_all_off.data()),
            0);
  EXPECT_EQ(read_all_off, expected);

  // (B) Single element selection -> H5D__create_piece_map_single.
  {
    std::array<hsize_t, 3> pt_coord = {4, 6, 8};
    ASSERT_GE(H5Sselect_elements(fspace.get(), H5S_SELECT_SET, 1,
                                 pt_coord.data()),
              0);
    std::array<hsize_t, 1> mdim = {1};
    ScopedSpace mspace(H5Screate_simple(1, mdim.data(), nullptr));
    int32_t val = 987654;
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, mspace.get(), fspace.get(),
                       dxpl_sel_on.get(), &val),
              0);
    int32_t out = -1;
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, mspace.get(), fspace.get(),
                      dxpl_sel_off.get(), &out),
              0);
    EXPECT_EQ(out, 987654);
    expected[4 * 63 + 6 * 9 + 8] = 987654;
  }

  // (C) Strided hyperslab spanning 3 chunks per axis while skipping middle
  // chunks so bounding box iterates over empty-intersection chunks
  // (H5D__create_piece_file_map_hyper + H5D__create_piece_mem_map_hyper + 1D).
  {
    std::array<hsize_t, 3> start = {0, 0, 0};
    std::array<hsize_t, 3> stride = {4, 6, 8};
    std::array<hsize_t, 3> count = {2, 2, 2};
    std::array<hsize_t, 3> block = {1, 1, 1};
    ASSERT_GE(H5Sselect_hyperslab(fspace.get(), H5S_SELECT_SET, start.data(),
                                  stride.data(), count.data(), block.data()),
              0);

    // Same-shape 3D memory space (multi-chunk H5S_SEL_HYPERSLABS in mem_map_hyper).
    std::array<hsize_t, 3> mdims3d = {5, 7, 9};
    ScopedSpace mspace_same(H5Screate_simple(3, mdims3d.data(), nullptr));
    ASSERT_GE(H5Sselect_hyperslab(mspace_same.get(), H5S_SELECT_SET,
                                  start.data(), stride.data(), count.data(),
                                  block.data()),
              0);
    std::vector<int32_t> mbuf_same(n_total, -1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, mspace_same.get(),
                      fspace.get(), dxpl_sel_on.get(), mbuf_same.data()),
              0);
    EXPECT_EQ(mbuf_same[0], expected[0]);
    EXPECT_EQ(mbuf_same[4 * 63 + 6 * 9 + 8], 987654);

    // Contiguous 1D regular block memory space (H5D__create_piece_mem_map_1d
    // multi-chunk).
    std::array<hsize_t, 1> mdim1d = {8};
    ScopedSpace mspace_1d(H5Screate_simple(1, mdim1d.data(), nullptr));
    std::array<int32_t, 8> out1d{};
    out1d.fill(-1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, mspace_1d.get(),
                      fspace.get(), dxpl_sel_off.get(), out1d.data()),
              0);
    EXPECT_EQ(out1d[0], expected[0]);
    EXPECT_EQ(out1d[7], 987654);

    // Non-matching 2D strided memory space -> fallback H5D__piece_mem_cb!
    std::array<hsize_t, 2> mdim2d = {4, 4};
    ScopedSpace mspace_2d(H5Screate_simple(2, mdim2d.data(), nullptr));
    std::array<hsize_t, 2> mstart = {0, 0};
    std::array<hsize_t, 2> mstride = {1, 2};
    std::array<hsize_t, 2> mcount = {4, 2};
    ASSERT_GE(H5Sselect_hyperslab(mspace_2d.get(), H5S_SELECT_SET,
                                  mstart.data(), mstride.data(), mcount.data(),
                                  nullptr),
              0);
    std::vector<int32_t> mbuf2d(16, -1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, mspace_2d.get(),
                      fspace.get(), dxpl_sel_on.get(), mbuf2d.data()),
              0);
    EXPECT_EQ(mbuf2d[0], expected[0]);
    EXPECT_EQ(mbuf2d[3 * 4 + 2], 987654);
  }

  // (D) Multi-point (>1) file point selection -> H5D__piece_file_cb +
  // single-chunk shape-same & 1D memory map.
  {
    const std::array<hsize_t, 9> pts = {
        0, 0, 0,  // chunk {0,0,0}
        0, 0, 1,  // chunk {0,0,0}
        2, 3, 4,  // chunk {1,1,1}
    };
    ASSERT_GE(H5Sselect_elements(fspace.get(), H5S_SELECT_SET, 3, pts.data()),
              0);
    std::array<hsize_t, 1> mdim = {3};
    ScopedSpace mspace_pts(H5Screate_simple(1, mdim.data(), nullptr));
    std::array<int32_t, 3> vals = {};
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, mspace_pts.get(),
                      fspace.get(), dxpl_sel_on.get(), vals.data()),
              0);
    EXPECT_EQ(vals[0], expected[0]);
    EXPECT_EQ(vals[1], expected[1]);
    EXPECT_EQ(vals[2], expected[2 * 63 + 3 * 9 + 4]);

    // Also exercise shape-same inside a single chunk ({0,0,0}..{1,2,3}).
    std::array<hsize_t, 3> start = {0, 0, 0};
    std::array<hsize_t, 3> count = {2, 2, 2};
    ASSERT_GE(H5Sselect_hyperslab(fspace.get(), H5S_SELECT_SET, start.data(),
                                  nullptr, count.data(), nullptr),
              0);
    ScopedSpace mspace_one_chunk(H5Screate_simple(3, dims.data(), nullptr));
    ASSERT_GE(H5Sselect_hyperslab(mspace_one_chunk.get(), H5S_SELECT_SET,
                                  start.data(), nullptr, count.data(), nullptr),
              0);
    std::vector<int32_t> mbuf(n_total, -1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, mspace_one_chunk.get(),
                      fspace.get(), dxpl_sel_on.get(), mbuf.data()),
              0);
    EXPECT_EQ(mbuf[0], expected[0]);
    EXPECT_EQ(mbuf[1 * 63 + 1 * 9 + 1], expected[1 * 63 + 1 * 9 + 1]);

    // Same 1-chunk file hyperslab into 1D memory selection (1D single chunk path).
    std::array<hsize_t, 1> mdim8 = {8};
    ScopedSpace mspace8(H5Screate_simple(1, mdim8.data(), nullptr));
    std::array<int32_t, 8> buf8{};
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, mspace8.get(), fspace.get(),
                      dxpl_sel_off.get(), buf8.data()),
              0);
    EXPECT_EQ(buf8[0], expected[0]);

    // Empty (H5S_SEL_NONE) selection.
    ASSERT_GE(H5Sselect_none(fspace.get()), 0);
    ASSERT_GE(H5Sselect_none(mspace8.get()), 0);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, mspace8.get(), fspace.get(),
                      dxpl_sel_on.get(), buf8.data()),
              0);
  }

  // (E) 1D chunked dataset with non-shape-same strided file selection and
  // contiguous 1D regular single-block memory space -> hits
  // H5D__create_piece_mem_map_1d for both single-chunk and multi-chunk!
  {
    std::array<hsize_t, 1> dims1 = {16};
    std::array<hsize_t, 1> cdims1 = {4};
    ScopedSpace fsp1(H5Screate_simple(1, dims1.data(), nullptr));
    ScopedPlist dcpl1 = MakeChunkedDcpl(cdims1);
    ScopedDataset d1(H5Dcreate2(file.get(), "map1d", H5T_NATIVE_INT32,
                                fsp1.get(), H5P_DEFAULT, dcpl1.get(),
                                dapl.get()));
    ASSERT_TRUE(d1.valid());

    // Single-chunk 1D strided selection (indices 0, 2 -> chunk 0 only).
    {
      std::array<hsize_t, 1> start = {0};
      std::array<hsize_t, 1> stride = {2};
      std::array<hsize_t, 1> count = {2};
      std::array<hsize_t, 1> mdim = {2};
      ASSERT_GE(H5Sselect_hyperslab(fsp1.get(), H5S_SELECT_SET, start.data(),
                                    stride.data(), count.data(), nullptr),
                0);
      ScopedSpace msp1(H5Screate_simple(1, mdim.data(), nullptr));
      std::array<int32_t, 2> w1 = {111, 222};
      ASSERT_GE(H5Dwrite(d1.get(), H5T_NATIVE_INT32, msp1.get(), fsp1.get(),
                         dxpl_sel_off.get(), w1.data()),
                0);
      std::array<int32_t, 2> r1 = {-1, -1};
      ASSERT_GE(H5Dread(d1.get(), H5T_NATIVE_INT32, msp1.get(), fsp1.get(),
                        dxpl_sel_on.get(), r1.data()),
                0);
      EXPECT_THAT(r1, ElementsAre(111, 222));
    }

    // Multi-chunk 1D strided selection (indices 0, 3, 6, 9, 12 -> chunks 0..3).
    {
      std::array<hsize_t, 1> start = {0};
      std::array<hsize_t, 1> stride = {3};
      std::array<hsize_t, 1> count = {5};
      std::array<hsize_t, 1> mdim = {5};
      ASSERT_GE(H5Sselect_hyperslab(fsp1.get(), H5S_SELECT_SET, start.data(),
                                    stride.data(), count.data(), nullptr),
                0);
      ScopedSpace msp5(H5Screate_simple(1, mdim.data(), nullptr));
      std::array<int32_t, 5> w5 = {10, 20, 30, 40, 50};
      ASSERT_GE(H5Dwrite(d1.get(), H5T_NATIVE_INT32, msp5.get(), fsp1.get(),
                         dxpl_sel_on.get(), w5.data()),
                0);
      std::array<int32_t, 5> r5 = {};
      ASSERT_GE(H5Dread(d1.get(), H5T_NATIVE_INT32, msp5.get(), fsp1.get(),
                        dxpl_sel_off.get(), r5.data()),
                0);
      EXPECT_THAT(r5, ElementsAre(10, 20, 30, 40, 50));
    }
  }

  // (F) 2D hyperslab file selection paired with H5S_SEL_POINTS 2D memory
  // selection -> hits H5S_SEL_POINTS branch in H5D__piece_mem_cb!
  {
    std::array<hsize_t, 2> dims2 = {4, 4};
    std::array<hsize_t, 2> cdims2 = {2, 2};
    ScopedSpace fsp2(H5Screate_simple(2, dims2.data(), nullptr));
    ScopedSpace msp2(H5Screate_simple(2, dims2.data(), nullptr));
    ScopedPlist dcpl2 = MakeChunkedDcpl(cdims2);
    ScopedDataset d2(H5Dcreate2(file.get(), "mem_pts2d", H5T_NATIVE_INT32,
                                fsp2.get(), H5P_DEFAULT, dcpl2.get(),
                                dapl.get()));
    ASSERT_TRUE(d2.valid());

    std::array<hsize_t, 2> start = {0, 0};
    std::array<hsize_t, 2> count = {2, 2};
    ASSERT_GE(H5Sselect_hyperslab(fsp2.get(), H5S_SELECT_SET, start.data(),
                                  nullptr, count.data(), nullptr),
              0);
    const std::array<hsize_t, 8> mpts = {0, 0, 1, 3, 2, 1, 3, 2};
    ASSERT_GE(H5Sselect_elements(msp2.get(), H5S_SELECT_SET, 4, mpts.data()),
              0);
    std::vector<int32_t> mbuf(16, -1);
    mbuf[0 * 4 + 0] = 41;
    mbuf[1 * 4 + 3] = 42;
    mbuf[2 * 4 + 1] = 43;
    mbuf[3 * 4 + 2] = 44;
    ASSERT_GE(H5Dwrite(d2.get(), H5T_NATIVE_INT32, msp2.get(), fsp2.get(),
                       dxpl_sel_on.get(), mbuf.data()),
              0);

    std::vector<int32_t> outbuf(16, -1);
    ASSERT_GE(H5Dread(d2.get(), H5T_NATIVE_INT32, msp2.get(), fsp2.get(),
                      dxpl_sel_off.get(), outbuf.data()),
              0);
    EXPECT_EQ(outbuf[0 * 4 + 0], 41);
    EXPECT_EQ(outbuf[1 * 4 + 3], 42);
    EXPECT_EQ(outbuf[2 * 4 + 1], 43);
    EXPECT_EQ(outbuf[3 * 4 + 2], 44);
  }
}

TEST(H5DchunkTest, SelectionIoCausesAndMultiDatasetIoWithTypeConversion) {
  // 1. Page-buffer selection I/O fallback + chunk cache + filter causes.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_GE(
        H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE, false, 1),
        0);
    ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 4096), 0);

    ScopedPlist fapl_pb = MakeCoreFapl();
    ASSERT_GE(H5Pset_page_buffer_size(fapl_pb.get(), 8192, 0, 0), 0);
    ScopedFile pb_file(
        H5Fcreate("pb_causes.h5", H5F_ACC_TRUNC, fcpl.get(), fapl_pb.get()));
    ASSERT_TRUE(pb_file.valid());

    std::array<hsize_t, 1> dims = {16};
    std::array<hsize_t, 1> cdims = {4};
    ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ScopedPlist dapl_nocache(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_chunk_cache(dapl_nocache.get(), 0, 0, 0.0), 0);
    ScopedDataset ds_pb(H5Dcreate2(pb_file.get(), "ds_pb", H5T_NATIVE_INT32,
                                   space.get(), H5P_DEFAULT, dcpl.get(),
                                   dapl_nocache.get()));
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_GE(H5Pset_selection_io(dxpl.get(), H5D_SELECTION_IO_MODE_ON), 0);
    std::vector<int32_t> vals(16, 7);
    ASSERT_GE(H5Dwrite(ds_pb.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       dxpl.get(), vals.data()),
              0);
    uint32_t cause = 0;
    ASSERT_GE(H5Pget_no_selection_io_cause(dxpl.get(), &cause), 0);
    EXPECT_NE(cause & H5D_SEL_IO_PAGE_BUFFER, 0u);
  }

  // 2. Dataset filter and chunk cache no-selection-IO causes.
  {
    ScopedFile file = CreateInMemoryFile("causes.h5");
    ASSERT_TRUE(file.valid());
    std::array<hsize_t, 1> dims = {8};
    std::array<hsize_t, 1> cdims = {4};
    ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));

    ScopedPlist dcpl_filt = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_deflate(dcpl_filt.get(), 6), 0);
    ScopedDataset ds_filt(H5Dcreate2(file.get(), "ds_filt", H5T_NATIVE_INT32,
                                     space.get(), H5P_DEFAULT, dcpl_filt.get(),
                                     H5P_DEFAULT));

    ScopedPlist dcpl_cache = MakeChunkedDcpl(cdims);
    ScopedDataset ds_cache(H5Dcreate2(file.get(), "ds_cache", H5T_NATIVE_INT32,
                                      space.get(), H5P_DEFAULT,
                                      dcpl_cache.get(), H5P_DEFAULT));

    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_GE(H5Pset_selection_io(dxpl.get(), H5D_SELECTION_IO_MODE_ON), 0);
    std::vector<int32_t> vals(8, 12);

    ASSERT_GE(H5Dwrite(ds_filt.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       dxpl.get(), vals.data()),
              0);
    uint32_t cause = 0;
    ASSERT_GE(H5Pget_no_selection_io_cause(dxpl.get(), &cause), 0);
    EXPECT_NE(cause & H5D_SEL_IO_DATASET_FILTER, 0u);

    ASSERT_GE(H5Dwrite(ds_cache.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       dxpl.get(), vals.data()),
              0);
    cause = 0;
    ASSERT_GE(H5Pget_no_selection_io_cause(dxpl.get(), &cause), 0);
    EXPECT_NE(cause & H5D_SEL_IO_CHUNK_CACHE, 0u);
  }

  // 3. Multi-dataset read/write (H5Dwrite_multi / H5Dread_multi) across >8
  // uncacheable chunks with in-place AND out-of-place type conversion, plus
  // reading nonexistent chunks via multi-dataset selection I/O!
  {
    ScopedFile file = CreateInMemoryFile("multi_dset.h5");
    ASSERT_TRUE(file.valid());

    // 10 chunks of 4 int32s each -> 40 elements per dataset (>8 pieces total).
    std::array<hsize_t, 1> dims = {40};
    std::array<hsize_t, 1> cdims = {4};
    ScopedSpace space1(H5Screate_simple(1, dims.data(), nullptr));
    ScopedSpace space2(H5Screate_simple(1, dims.data(), nullptr));
    ScopedSpace space3(H5Screate_simple(1, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    int32_t fill_v = -42;
    ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_INT32, &fill_v), 0);
    ScopedPlist dapl_nocache(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_chunk_cache(dapl_nocache.get(), 0, 0, 0.0), 0);

    ScopedDataset d1(H5Dcreate2(file.get(), "d1", H5T_NATIVE_INT32,
                                space1.get(), H5P_DEFAULT, dcpl.get(),
                                dapl_nocache.get()));
    ScopedDataset d2(H5Dcreate2(file.get(), "d2", H5T_NATIVE_INT64,
                                space2.get(), H5P_DEFAULT, dcpl.get(),
                                dapl_nocache.get()));
    // Leave d3 empty until read to test nonexistent chunks in H5Dread_multi.
    ScopedDataset d3(H5Dcreate2(file.get(), "d3", H5T_NATIVE_INT32,
                                space3.get(), H5P_DEFAULT, dcpl.get(),
                                dapl_nocache.get()));
    ASSERT_TRUE(d1.valid());
    ASSERT_TRUE(d2.valid());
    ASSERT_TRUE(d3.valid());

    // Prepare write data with int64 memory buffer -> int32 disk (in-place when
    // modify_write_buf=true and mem >= file), and int32 mem -> int64 disk
    // (out-of-place when mem < file).
    std::vector<int64_t> wbuf1(40);
    std::vector<int32_t> wbuf2(40);
    for (int i = 0; i < 40; ++i) {
      wbuf1[i] = 1000 + i;
      wbuf2[i] = 2000 + i;
    }

    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_GE(H5Pset_selection_io(dxpl.get(), H5D_SELECTION_IO_MODE_ON), 0);
    ASSERT_GE(H5Pset_modify_write_buf(dxpl.get(), true), 0);

    std::array<hid_t, 2> wdsets = {d1.get(), d2.get()};
    std::array<hid_t, 2> wmtypes = {H5T_NATIVE_INT64, H5T_NATIVE_INT32};
    std::array<hid_t, 2> wmspaces = {H5S_ALL, H5S_ALL};
    std::array<hid_t, 2> wfspaces = {H5S_ALL, H5S_ALL};
    std::array<const void*, 2> wbufs = {wbuf1.data(), wbuf2.data()};
    ASSERT_GE(H5Dwrite_multi(2, wdsets.data(), wmtypes.data(), wmspaces.data(),
                             wfspaces.data(), dxpl.get(), wbufs.data()),
              0);

    std::vector<int64_t> rbuf1(40, -1);
    std::vector<int32_t> rbuf2(40, -1);
    std::vector<int32_t> rbuf3(40, -1);
    std::array<hid_t, 3> rdsets = {d1.get(), d2.get(), d3.get()};
    std::array<hid_t, 3> rmtypes = {H5T_NATIVE_INT64, H5T_NATIVE_INT32,
                                    H5T_NATIVE_INT32};
    std::array<hid_t, 3> rmspaces = {H5S_ALL, H5S_ALL, H5S_ALL};
    std::array<hid_t, 3> rfspaces = {H5S_ALL, H5S_ALL, H5S_ALL};
    std::array<void*, 3> rbufs = {rbuf1.data(), rbuf2.data(), rbuf3.data()};
    ASSERT_GE(H5Dread_multi(3, rdsets.data(), rmtypes.data(), rmspaces.data(),
                            rfspaces.data(), dxpl.get(), rbufs.data()),
              0);

    for (int i = 0; i < 40; ++i) {
      EXPECT_EQ(rbuf1[i], 1000 + i);
      EXPECT_EQ(rbuf2[i], 2000 + i);
      EXPECT_EQ(rbuf3[i], -42);
    }

    // Same multi-dset read with Selection I/O OFF.
    std::fill(rbuf1.begin(), rbuf1.end(), -1);
    std::fill(rbuf2.begin(), rbuf2.end(), -1);
    std::fill(rbuf3.begin(), rbuf3.end(), -1);
    ASSERT_GE(H5Pset_selection_io(dxpl.get(), H5D_SELECTION_IO_MODE_OFF), 0);
    ASSERT_GE(H5Dread_multi(3, rdsets.data(), rmtypes.data(), rmspaces.data(),
                            rfspaces.data(), dxpl.get(), rbufs.data()),
              0);
    for (int i = 0; i < 40; ++i) {
      EXPECT_EQ(rbuf1[i], 1000 + i);
      EXPECT_EQ(rbuf2[i], 2000 + i);
      EXPECT_EQ(rbuf3[i], -42);
    }

    // Also run H5Dwrite_multi on already-allocated (EARLY) uncacheable
    // datasets with Selection I/O ON -> exercises mdset write array insertion
    // in H5D__chunk_write!
    ScopedPlist dcpl_early = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_alloc_time(dcpl_early.get(), H5D_ALLOC_TIME_EARLY), 0);
    ScopedDataset d1e(H5Dcreate2(file.get(), "d1e", H5T_NATIVE_INT32,
                                 space1.get(), H5P_DEFAULT, dcpl_early.get(),
                                 dapl_nocache.get()));
    ScopedDataset d2e(H5Dcreate2(file.get(), "d2e", H5T_NATIVE_INT64,
                                 space2.get(), H5P_DEFAULT, dcpl_early.get(),
                                 dapl_nocache.get()));
    ASSERT_TRUE(d1e.valid());
    ASSERT_TRUE(d2e.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl.get(), H5D_SELECTION_IO_MODE_ON), 0);
    std::array<hid_t, 2> wedsets = {d1e.get(), d2e.get()};
    ASSERT_GE(H5Dwrite_multi(2, wedsets.data(), wmtypes.data(),
                             wmspaces.data(), wfspaces.data(), dxpl.get(),
                             wbufs.data()),
              0);

    // Single-dataset Selection I/O ON write on H5D_ALLOC_TIME_INCR +
    // H5D_FILL_TIME_NEVER + rdcc_nbytes = 0: cacheable == false -> allocates
    // unallocated chunks directly inside H5D__chunk_write!
    ScopedPlist dcpl_incr_nf = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_alloc_time(dcpl_incr_nf.get(), H5D_ALLOC_TIME_INCR), 0);
    ASSERT_GE(H5Pset_fill_time(dcpl_incr_nf.get(), H5D_FILL_TIME_NEVER), 0);
    std::array<hsize_t, 1> dims8 = {8};
    ScopedSpace space8(H5Screate_simple(1, dims8.data(), nullptr));
    ScopedDataset d_incr_nf(H5Dcreate2(file.get(), "incr_nofill_sel",
                                       H5T_NATIVE_INT32, space8.get(),
                                       H5P_DEFAULT, dcpl_incr_nf.get(),
                                       dapl_nocache.get()));
    ASSERT_TRUE(d_incr_nf.valid());
    std::vector<int32_t> w8 = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_GE(H5Dwrite(d_incr_nf.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       dxpl.get(), w8.data()),
              0);
  }
}

// ---------------------------------------------------------------------------
// 4. Uncacheable Sieve Buffer Fallback & Nonexistent Fill Value ReadVV
// ---------------------------------------------------------------------------

TEST(H5DchunkTest, UncacheableSieveFallbackAndNonexistentFillReadvv) {
  ScopedFile file = CreateInMemoryFile("nonexistent_readvv.h5");
  ASSERT_TRUE(file.valid());

  ScopedPlist dxpl_off(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(H5Pset_selection_io(dxpl_off.get(), H5D_SELECTION_IO_MODE_OFF), 0);

  // Use rdcc_nbytes = 16 with 32-byte chunks (8 int32s) so chunks are too large
  // to stay cached -> hits uncacheable ctg_store sieve path and
  // H5D__nonexistent_readvv!
  std::array<hsize_t, 2> dims = {4, 8};
  std::array<hsize_t, 2> cdims = {2, 4};  // 8 elements = 32 bytes.
  ScopedSpace space(H5Screate_simple(2, dims.data(), nullptr));
  ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_GE(H5Pset_chunk_cache(dapl.get(), 17, 16, 0.5), 0);

  // 1. Defined fill value + FILL_TIME_IFSET -> H5D__nonexistent_readvv fills
  // only selected chunks cleanly while leaving existing chunks intact.
  ScopedPlist dcpl = MakeChunkedDcpl(cdims);
  int32_t fill_val = 555;
  ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_INT32, &fill_val), 0);
  ASSERT_GE(H5Pset_fill_time(dcpl.get(), H5D_FILL_TIME_IFSET), 0);
  ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_LATE), 0);

  ScopedDataset dset(H5Dcreate2(file.get(), "uncacheable", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                dapl.get()));
  ASSERT_TRUE(dset.valid());

  // Partial write to chunk {0, 0} when not yet allocated: write_op + fill
  // briefly caches & immediately flushes (or fills) the new chunk via temp cache
  // path (ent->chunk_block.offset HADDR_UNDEF unlock flush).
  std::array<hsize_t, 2> start = {0, 0};
  std::array<hsize_t, 2> count = {1, 2};
  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, start.data(),
                                nullptr, count.data(), nullptr),
            0);
  std::array<hsize_t, 1> mdim2 = {2};
  ScopedSpace mspace2(H5Screate_simple(1, mdim2.data(), nullptr));
  std::array<int32_t, 2> two_vals = {11, 22};
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, mspace2.get(), space.get(),
                     dxpl_off.get(), two_vals.data()),
            0);

  // Partial write into chunk {0, 0} at {1, 0} hits existing uncacheable chunk.
  start = {1, 0};
  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, start.data(),
                                nullptr, count.data(), nullptr),
            0);
  two_vals = {33, 44};
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, mspace2.get(), space.get(),
                     dxpl_off.get(), two_vals.data()),
            0);

  // Multi-chunk full read via H5S_ALL with Selection I/O OFF hits both the
  // uncacheable ctg_store sieve path for chunk {0, 0} and H5D__nonexistent_readvv
  // for missing chunks {0, 1}, {1, 0}, {1, 1}.
  std::vector<int32_t> out(32, -999);
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    dxpl_off.get(), out.data()),
            0);
  EXPECT_EQ(out[0], 11);
  EXPECT_EQ(out[1], 22);
  EXPECT_EQ(out[2], 555);
  EXPECT_EQ(out[8], 33);
  EXPECT_EQ(out[9], 44);
  EXPECT_EQ(out[4 * 8 - 1], 555);

  // Single-element uncacheable read & write across missing/existing chunks + type
  // conversion into nonexistent readvv.
  {
    std::array<hsize_t, 2> pt = {3, 7};
    ASSERT_GE(H5Sselect_elements(space.get(), H5S_SELECT_SET, 1, pt.data()), 0);
    std::array<hsize_t, 1> mdim1 = {1};
    ScopedSpace mspace1(H5Screate_simple(1, mdim1.data(), nullptr));
    int64_t converted_fill = 0;
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT64, mspace1.get(), space.get(),
                      dxpl_off.get(), &converted_fill),
              0);
    EXPECT_EQ(converted_fill, 555);
  }

  // 2. FILL_TIME_NEVER skips missing chunks cleanly, preserving caller sentinel.
  ScopedPlist dcpl_never = MakeChunkedDcpl(cdims);
  ASSERT_GE(H5Pset_fill_time(dcpl_never.get(), H5D_FILL_TIME_NEVER), 0);
  ScopedDataset dset_never(H5Dcreate2(file.get(), "never_fill",
                                      H5T_NATIVE_INT32, space.get(),
                                      H5P_DEFAULT, dcpl_never.get(),
                                      dapl.get()));
  ASSERT_TRUE(dset_never.valid());
  std::vector<int32_t> sentinel_buf(32, 777);
  ASSERT_GE(H5Dread(dset_never.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    dxpl_off.get(), sentinel_buf.data()),
            0);
  EXPECT_EQ(sentinel_buf[0], 777);
  EXPECT_EQ(sentinel_buf[31], 777);

  // Also test partial write with FILL_TIME_NEVER on uncacheable dataset (takes
  // the non-cached direct ctg_store write path!).
  start = {0, 0};
  count = {2, 4};
  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, start.data(),
                                nullptr, count.data(), nullptr),
            0);
  std::vector<int32_t> eight_vals = {1, 2, 3, 4, 5, 6, 7, 8};
  std::array<hsize_t, 1> mdim8 = {8};
  ScopedSpace mspace8(H5Screate_simple(1, mdim8.data(), nullptr));
  ASSERT_GE(H5Dwrite(dset_never.get(), H5T_NATIVE_INT32, mspace8.get(),
                     space.get(), dxpl_off.get(), eight_vals.data()),
            0);
  std::fill(sentinel_buf.begin(), sentinel_buf.end(), 777);
  ASSERT_GE(H5Dread(dset_never.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    dxpl_off.get(), sentinel_buf.data()),
            0);
  EXPECT_EQ(sentinel_buf[0], 1);
  EXPECT_EQ(sentinel_buf[3], 4);
  EXPECT_EQ(sentinel_buf[7], 777);
  EXPECT_EQ(sentinel_buf[8], 5);

  // 3. Variable-length string fill value read from nonexistent chunks.
  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);
  const char* vl_fill = "default_str";
  ScopedPlist dcpl_vl = MakeChunkedDcpl(cdims);
  ASSERT_GE(H5Pset_fill_value(dcpl_vl.get(), vl_str.get(), &vl_fill), 0);
  ScopedDataset dset_vl(H5Dcreate2(file.get(), "vl_nonexistent", vl_str.get(),
                                   space.get(), H5P_DEFAULT, dcpl_vl.get(),
                                   dapl.get()));
  ASSERT_TRUE(dset_vl.valid());
  std::vector<char*> vl_read(32, nullptr);
  ASSERT_GE(H5Dread(dset_vl.get(), vl_str.get(), H5S_ALL, H5S_ALL,
                    dxpl_off.get(), vl_read.data()),
            0);
  ScopedSpace full_sp(H5Dget_space(dset_vl.get()));
  auto reclaim_vl = absl::MakeCleanup([&]() {
    H5Treclaim(vl_str.get(), full_sp.get(), H5P_DEFAULT, vl_read.data());
  });
  ASSERT_NE(vl_read[0], nullptr);
  EXPECT_STREQ(vl_read[0], "default_str");
  ASSERT_NE(vl_read[31], nullptr);
  EXPECT_STREQ(vl_read[31], "default_str");

  // 4. H5D_ALLOC_TIME_INCR + rdcc_nbytes = 0 + partial write to chunk {0, 0}
  // ONLY (leaves remaining chunks HADDR_UNDEF while is_space_alloc = true!):
  // reading all chunks exercises both H5D__nonexistent_readvv (Selection I/O
  // OFF) and selection I/O nonexistent fill read (Selection I/O ON)!
  {
    ScopedPlist dxpl_on(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_GE(H5Pset_selection_io(dxpl_on.get(), H5D_SELECTION_IO_MODE_ON), 0);

    ScopedPlist dcpl_incr = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_fill_value(dcpl_incr.get(), H5T_NATIVE_INT32, &fill_val),
              0);
    ASSERT_GE(H5Pset_fill_time(dcpl_incr.get(), H5D_FILL_TIME_ALLOC), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl_incr.get(), H5D_ALLOC_TIME_INCR), 0);
    ScopedDataset d_incr(H5Dcreate2(file.get(), "incr_nonex", H5T_NATIVE_INT32,
                                    space.get(), H5P_DEFAULT, dcpl_incr.get(),
                                    dapl.get()));
    ASSERT_TRUE(d_incr.valid());

    start = {0, 0};
    count = {1, 2};
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, start.data(),
                                  nullptr, count.data(), nullptr),
              0);
    two_vals = {123, 456};
    ASSERT_GE(H5Dwrite(d_incr.get(), H5T_NATIVE_INT32, mspace2.get(),
                       space.get(), dxpl_off.get(), two_vals.data()),
              0);

    std::vector<int32_t> r_off(32, -1);
    ASSERT_GE(H5Dread(d_incr.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      dxpl_off.get(), r_off.data()),
              0);
    EXPECT_EQ(r_off[0], 123);
    EXPECT_EQ(r_off[31], 555);

    std::vector<int32_t> r_on(32, -1);
    ASSERT_GE(H5Dread(d_incr.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      dxpl_on.get(), r_on.data()),
              0);
    EXPECT_EQ(r_on[1], 456);
    EXPECT_EQ(r_on[20], 555);
  }

  // 5. VLEN string H5D_ALLOC_TIME_INCR with rdcc_nbytes = 0 (write 1 element so
  // remaining chunks are unallocated, hitting has_vlen_fill_type in
  // H5D__nonexistent_readvv_cb!) AND cached RDCC VLEN fill (H5D__chunk_lock).
  {
    ScopedPlist dcpl_vl_incr = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_fill_value(dcpl_vl_incr.get(), vl_str.get(), &vl_fill), 0);
    ASSERT_GE(H5Pset_fill_time(dcpl_vl_incr.get(), H5D_FILL_TIME_ALLOC), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl_vl_incr.get(), H5D_ALLOC_TIME_INCR), 0);
    ScopedDataset d_vl_incr(H5Dcreate2(file.get(), "vl_incr_nonex",
                                       vl_str.get(), space.get(), H5P_DEFAULT,
                                       dcpl_vl_incr.get(), dapl.get()));
    ASSERT_TRUE(d_vl_incr.valid());

    start = {0, 0};
    count = {1, 1};
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, start.data(),
                                  nullptr, count.data(), nullptr),
              0);
    std::array<hsize_t, 1> m1 = {1};
    ScopedSpace msp1(H5Screate_simple(1, m1.data(), nullptr));
    const char* s0 = "first_vl";
    ASSERT_GE(H5Dwrite(d_vl_incr.get(), vl_str.get(), msp1.get(), space.get(),
                       dxpl_off.get(), &s0),
              0);
    std::vector<char*> vl_out(32, nullptr);
    ASSERT_GE(H5Dread(d_vl_incr.get(), vl_str.get(), H5S_ALL, H5S_ALL,
                      dxpl_off.get(), vl_out.data()),
              0);
    auto reclaim_out = absl::MakeCleanup([&]() {
      H5Treclaim(vl_str.get(), full_sp.get(), H5P_DEFAULT, vl_out.data());
    });
    EXPECT_STREQ(vl_out[0], "first_vl");
    EXPECT_STREQ(vl_out[31], "default_str");

    // Default-cached VLEN partial write to unallocated chunk -> H5D__chunk_lock
    // VL fill init!
    ScopedDataset d_vl_cached(H5Dcreate2(file.get(), "vl_cached_fill",
                                         vl_str.get(), space.get(), H5P_DEFAULT,
                                         dcpl_vl_incr.get(), H5P_DEFAULT));
    ASSERT_TRUE(d_vl_cached.valid());
    start = {2, 4};
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, start.data(),
                                  nullptr, count.data(), nullptr),
              0);
    const char* s_c = "cached_vl";
    ASSERT_GE(H5Dwrite(d_vl_cached.get(), vl_str.get(), msp1.get(), space.get(),
                       dxpl_off.get(), &s_c),
              0);
    std::vector<char*> vl_cached_out(32, nullptr);
    ASSERT_GE(H5Dread(d_vl_cached.get(), vl_str.get(), H5S_ALL, H5S_ALL,
                      dxpl_off.get(), vl_cached_out.data()),
              0);
    auto reclaim_cached = absl::MakeCleanup([&]() {
      H5Treclaim(vl_str.get(), full_sp.get(), H5P_DEFAULT, vl_cached_out.data());
    });
    EXPECT_STREQ(vl_cached_out[2 * 8 + 4], "cached_vl");
    EXPECT_STREQ(vl_cached_out[2 * 8 + 5], "default_str");
  }
}

// ---------------------------------------------------------------------------
// 5. RDCC Cache Mechanics (Hits, Misses, Relax, w0 Prune, Collisions, Resize)
// ---------------------------------------------------------------------------

TEST(H5DchunkTest, ChunkCacheEvictionPreemptionW0AndResizeUpdate) {
  ScopedFile file = CreateInMemoryFile("rdcc_cache.h5");
  ASSERT_TRUE(file.valid());
  ScopedPlist dxpl_off(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(H5Pset_selection_io(dxpl_off.get(), H5D_SELECTION_IO_MODE_OFF), 0);

  // Test w0 = 0.0, 0.5, and 1.0 preemption policies over a 1D chunked dataset
  // holding at most 2 chunks of 16 bytes (nbytes = 32, nslots = 13).
  constexpr std::array<double, 3> kW0Values = {0.0, 0.5, 1.0};
  for (size_t idx = 0; idx < kW0Values.size(); ++idx) {
    const std::string name = "w0_" + std::to_string(idx);
    std::array<hsize_t, 1> dims = {16};
    std::array<hsize_t, 1> cdims = {4};  // 4 chunks x 16 bytes each.
    ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_chunk_cache(dapl.get(), 13, 32, kW0Values[idx]), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), name.c_str(), H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  dapl.get()));
    ASSERT_TRUE(dset.valid());

    // Chunk 0: completely written (wr_count == 0).
    std::array<hsize_t, 1> start = {0};
    std::array<hsize_t, 1> count = {4};
    std::array<hsize_t, 1> mdim4 = {4};
    ScopedSpace m4(H5Screate_simple(1, mdim4.data(), nullptr));
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, start.data(),
                                  nullptr, count.data(), nullptr),
              0);
    std::array<int32_t, 4> c0 = {1, 2, 3, 4};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, m4.get(), space.get(),
                       dxpl_off.get(), c0.data()),
              0);

    // Chunk 1: partially written (wr_count > 0).
    start = {4};
    count = {2};
    std::array<hsize_t, 1> mdim2 = {2};
    ScopedSpace m2(H5Screate_simple(1, mdim2.data(), nullptr));
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, start.data(),
                                  nullptr, count.data(), nullptr),
              0);
    std::array<int32_t, 2> c1 = {10, 11};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, m2.get(), space.get(),
                       dxpl_off.get(), c1.data()),
              0);

    // Hit chunk 0 in cache while it is not the tail (ent->next != nullptr) to
    // trigger LRU move-to-tail.
    start = {0};
    count = {2};
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, start.data(),
                                  nullptr, count.data(), nullptr),
              0);
    c0[0] = 99;
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, m2.get(), space.get(),
                       dxpl_off.get(), c0.data()),
              0);

    // Insert Chunk 2 and Chunk 3 to force H5D__chunk_cache_prune across both
    // fully-written and partially-written entries.
    start = {8};
    count = {4};
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, start.data(),
                                  nullptr, count.data(), nullptr),
              0);
    std::array<int32_t, 4> c2 = {20, 21, 22, 23};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, m4.get(), space.get(),
                       dxpl_off.get(), c2.data()),
              0);

    start = {12};
    count = {4};
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, start.data(),
                                  nullptr, count.data(), nullptr),
              0);
    std::array<int32_t, 4> c3 = {30, 31, 32, 33};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, m4.get(), space.get(),
                       dxpl_off.get(), c3.data()),
              0);

    // Re-read Chunk 0 completely via miss -> full read updates rd_count, then
    // overwrite Chunk 0 completely to exercise relax = true overwrite.
    start = {0};
    count = {4};
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, start.data(),
                                  nullptr, count.data(), nullptr),
              0);
    std::array<int32_t, 4> r0{};
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, m4.get(), space.get(),
                      dxpl_off.get(), r0.data()),
              0);
    EXPECT_THAT(r0, ElementsAre(99, 2, 3, 4));

    std::array<int32_t, 4> overwrite = {50, 51, 52, 53};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, m4.get(), space.get(),
                       dxpl_off.get(), overwrite.data()),
              0);
  }

  // Test hash-slot collisions (nslots = 1 with enough nbytes for 4 chunks) AND
  // multi-D H5Dset_extent triggering H5D__chunk_update_cache + tmp_head
  // re-hashing collision eviction!
  {
    std::array<hsize_t, 2> dims = {4, 4};
    std::array<hsize_t, 2> max_dims = {H5S_UNLIMITED, H5S_UNLIMITED};
    std::array<hsize_t, 2> cdims = {2, 2};
    ScopedSpace space(H5Screate_simple(2, dims.data(), max_dims.data()));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);

    // 1 slot -> every chunk hashes to slot 0 (forced collision preemption).
    ScopedPlist dapl_1slot(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_chunk_cache(dapl_1slot.get(), 1, 1024, 0.5), 0);
    ScopedDataset d_collide(H5Dcreate2(file.get(), "collide_1",
                                       H5T_NATIVE_INT32, space.get(),
                                       H5P_DEFAULT, dcpl.get(),
                                       dapl_1slot.get()));
    ASSERT_TRUE(d_collide.valid());

    std::array<hsize_t, 2> hs = {0, 0};
    std::array<hsize_t, 2> cnt = {2, 2};
    std::array<int32_t, 4> buf = {101, 102, 103, 104};
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, hs.data(),
                                  nullptr, cnt.data(), nullptr),
              0);
    ScopedSpace m4(H5Screate_simple(2, cnt.data(), nullptr));
    ASSERT_GE(H5Dwrite(d_collide.get(), H5T_NATIVE_INT32, m4.get(), space.get(),
                       dxpl_off.get(), buf.data()),
              0);

    // Writing chunk {2, 2} collides at slot 0 and preempts dirty chunk {0, 0}.
    hs = {2, 2};
    buf = {201, 202, 203, 204};
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, hs.data(),
                                  nullptr, cnt.data(), nullptr),
              0);
    ASSERT_GE(H5Dwrite(d_collide.get(), H5T_NATIVE_INT32, m4.get(), space.get(),
                       dxpl_off.get(), buf.data()),
              0);

    // Second dataset with nslots = 2 (large enough to hold multiple cached
    // chunks, small enough that resizing 2D extent causes new hashes to collide
    // inside tmp_head in H5D__chunk_update_cache).
    ScopedPlist dapl_resize(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_chunk_cache(dapl_resize.get(), 2, 4096, 0.5), 0);
    ScopedDataset d_resize(H5Dcreate2(file.get(), "resize_cache",
                                      H5T_NATIVE_INT32, space.get(),
                                      H5P_DEFAULT, dcpl.get(),
                                      dapl_resize.get()));
    ASSERT_TRUE(d_resize.valid());

    // Cache two dirty chunks at {0, 0} and {2, 0}.
    hs = {0, 0};
    buf = {1, 2, 3, 4};
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, hs.data(),
                                  nullptr, cnt.data(), nullptr),
              0);
    ASSERT_GE(H5Dwrite(d_resize.get(), H5T_NATIVE_INT32, m4.get(), space.get(),
                       dxpl_off.get(), buf.data()),
              0);

    hs = {2, 0};
    buf = {5, 6, 7, 8};
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, hs.data(),
                                  nullptr, cnt.data(), nullptr),
              0);
    ASSERT_GE(H5Dwrite(d_resize.get(), H5T_NATIVE_INT32, m4.get(), space.get(),
                       dxpl_off.get(), buf.data()),
              0);

    // Extend along dimension 1 past next power of 2 -> updates
    // scaled_encode_bits and triggers H5D__chunk_update_cache!
    std::array<hsize_t, 2> ext_dims = {10, 12};
    ASSERT_GE(H5Dset_extent(d_resize.get(), ext_dims.data()), 0);

    // Read full extent and verify cached + fill values survived rehash.
    ScopedSpace ext_sp(H5Dget_space(d_resize.get()));
    std::vector<int32_t> out(120, -1);
    ASSERT_GE(H5Dread(d_resize.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      dxpl_off.get(), out.data()),
              0);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[2 * 12 + 0], 5);

    // Third dataset with nslots = 5, dims = {8, 4} -> {8, 12} (bits[1]: 1 -> 3).
    // With LRU list ordered head={1,0} -> tail={1,1}, moving {1,0} from slot 2
    // to slot 3 pushes {1,1} onto tmp_tail; then visiting {1,1} (slot 3 -> 4)
    // pops it from tmp_tail (`tmp_tail == ent`, H5Dchunk.c:6820).
    std::array<hsize_t, 2> dims84 = {8, 4};
    ScopedSpace sp84(H5Screate_simple(2, dims84.data(), max_dims.data()));
    ScopedPlist dapl_tail(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_chunk_cache(dapl_tail.get(), 5, 4096, 0.5), 0);
    ScopedDataset d_tail(H5Dcreate2(file.get(), "resize_tmp_tail",
                                    H5T_NATIVE_INT32, sp84.get(), H5P_DEFAULT,
                                    dcpl.get(), dapl_tail.get()));
    ASSERT_TRUE(d_tail.valid());

    hs = {2, 2};  // scaled {1, 1}, old slot 3
    buf = {11, 12, 13, 14};
    ASSERT_GE(H5Sselect_hyperslab(sp84.get(), H5S_SELECT_SET, hs.data(),
                                  nullptr, cnt.data(), nullptr),
              0);
    ASSERT_GE(H5Dwrite(d_tail.get(), H5T_NATIVE_INT32, m4.get(), sp84.get(),
                       dxpl_off.get(), buf.data()),
              0);

    hs = {2, 0};  // scaled {1, 0}, old slot 2
    buf = {21, 22, 23, 24};
    ASSERT_GE(H5Sselect_hyperslab(sp84.get(), H5S_SELECT_SET, hs.data(),
                                  nullptr, cnt.data(), nullptr),
              0);
    ASSERT_GE(H5Dwrite(d_tail.get(), H5T_NATIVE_INT32, m4.get(), sp84.get(),
                       dxpl_off.get(), buf.data()),
              0);

    // Hit {1, 1} again to move it to rdcc->tail after {1, 0}.
    hs = {2, 2};
    buf = {31, 32, 33, 34};
    ASSERT_GE(H5Sselect_hyperslab(sp84.get(), H5S_SELECT_SET, hs.data(),
                                  nullptr, cnt.data(), nullptr),
              0);
    ASSERT_GE(H5Dwrite(d_tail.get(), H5T_NATIVE_INT32, m4.get(), sp84.get(),
                       dxpl_off.get(), buf.data()),
              0);

    std::array<hsize_t, 2> dims812 = {8, 12};
    ASSERT_GE(H5Dset_extent(d_tail.get(), dims812.data()), 0);
    std::vector<int32_t> out812(96, -1);
    ASSERT_GE(H5Dread(d_tail.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      dxpl_off.get(), out812.data()),
              0);
    EXPECT_EQ(out812[2 * 12 + 0], 21);
    EXPECT_EQ(out812[2 * 12 + 2], 31);
  }
}

// ---------------------------------------------------------------------------
// 6. Allocation (Early/Late/Fill), Partial Edge Chunks, Extend & Shrink/Prune
// ---------------------------------------------------------------------------

TEST(H5DchunkTest, EarlyAllocPartialEdgeChunksExtendAndPrune) {
  ScopedFile file = CreateInMemoryFile("edge_prune.h5");
  ASSERT_TRUE(file.valid());
  ScopedPlist dxpl_off(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(H5Pset_selection_io(dxpl_off.get(), H5D_SELECTION_IO_MODE_OFF), 0);

  // 1. Filtered 2D dataset starting at {0, 0}, then extended to {5, 7} with
  // ALLOC_TIME_EARLY and H5D_CHUNK_DONT_FILTER_PARTIAL_CHUNKS.
  // Chunk size is {4, 4}: at {5, 7}, chunk {0, 0} is FULL (filtered), while
  // {0, 1}, {1, 0}, {1, 1} are partial edge chunks (UNFILTERED!).
  {
    std::array<hsize_t, 2> init_dims = {0, 0};
    std::array<hsize_t, 2> max_dims = {H5S_UNLIMITED, H5S_UNLIMITED};
    std::array<hsize_t, 2> cdims = {4, 4};
    ScopedSpace space(H5Screate_simple(2, init_dims.data(), max_dims.data()));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);
    ASSERT_GE(H5Pset_chunk_opts(dcpl.get(), H5D_CHUNK_DONT_FILTER_PARTIAL_CHUNKS),
              0);
    ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_EARLY), 0);
    int32_t fill_val = 77;
    ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_INT32, &fill_val), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), "edge_early", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    // Extend from {0, 0} -> {5, 7}.
    std::array<hsize_t, 2> dims57 = {5, 7};
    ASSERT_GE(H5Dset_extent(dset.get(), dims57.data()), 0);

    // Dirty an edge chunk {1, 1} in RDCC cache without flushing yet, so
    // extending past {8, 8} covers H5D__chunk_update_old_edge_chunks both in
    // cache (prev_unfilt_chunk = true) and on disk (chunk {0, 1} & {1, 0}).
    ScopedSpace sp57(H5Dget_space(dset.get()));
    std::array<hsize_t, 2> hs = {4, 4};
    std::array<hsize_t, 2> cnt = {1, 3};
    ASSERT_GE(H5Sselect_hyperslab(sp57.get(), H5S_SELECT_SET, hs.data(),
                                  nullptr, cnt.data(), nullptr),
              0);
    std::array<hsize_t, 1> m3 = {3};
    ScopedSpace msp3(H5Screate_simple(1, m3.data(), nullptr));
    std::array<int32_t, 3> edge_data = {901, 902, 903};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, msp3.get(), sp57.get(),
                       dxpl_off.get(), edge_data.data()),
              0);

    // Extend {5, 7} -> {10, 10}: old edge chunks {0, 1}, {1, 0}, {1, 1} are now
    // interior full chunks and must get compressed!
    std::array<hsize_t, 2> dims10 = {10, 10};
    ASSERT_GE(H5Dset_extent(dset.get(), dims10.data()), 0);
    ASSERT_GE(H5Dflush(dset.get()), 0);

    // Verify chunk {0, 0} and {1, 1} are now filtered (filter_mask == 0,
    // size < 64 bytes for uniform fill + data), while {2, 2} is partial edge
    // (stored uncompressed = 64 bytes).
    unsigned fmask = 999;
    hsize_t csz = 0;
    std::array<hsize_t, 2> c11 = {4, 4};
    ASSERT_GE(
        H5Dget_chunk_info_by_coord(dset.get(), c11.data(), &fmask, nullptr, &csz),
        0);
    EXPECT_LT(csz, 64);

    // Read full 10x10 and verify cached edge write {901,902,903} + fill 77.
    std::vector<int32_t> buf10(100, -1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      dxpl_off.get(), buf10.data()),
              0);
    EXPECT_EQ(buf10[4 * 10 + 4], 901);
    EXPECT_EQ(buf10[4 * 10 + 5], 902);
    EXPECT_EQ(buf10[4 * 10 + 6], 903);
    EXPECT_EQ(buf10[0], 77);
    EXPECT_EQ(buf10[99], 77);

    // Now test H5D__chunk_prune_by_extent + H5D__chunk_prune_fill:
    // Write recognizable values into {0, 0}..{7, 7} and leave chunk {0, 1} dirty
    // in cache while also keeping {1, 1} on disk. Then shrink {10, 10} -> {6, 6}
    // so chunks {2, *} and {*, 2} are deleted (including evicting dirty cached
    // chunks!), while chunks {0, 1}, {1, 0}, {1, 1} transition from FULL
    // filtered chunks back to partial edge chunks (`new_unfilt_chunk = true`)!
    std::vector<int32_t> pat(100);
    std::iota(pat.begin(), pat.end(), 1);
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       dxpl_off.get(), pat.data()),
              0);
    ASSERT_GE(H5Dflush(dset.get()), 0);

    // Keep chunk {1, 1} AND edge chunk {2, 2} dirty in cache prior to shrink.
    ScopedSpace sp10(H5Dget_space(dset.get()));
    hs = {4, 4};
    std::array<hsize_t, 2> c1x1 = {1, 1};
    ASSERT_GE(H5Sselect_hyperslab(sp10.get(), H5S_SELECT_SET, hs.data(),
                                  nullptr, c1x1.data(), nullptr),
              0);
    std::array<hsize_t, 1> m1 = {1};
    ScopedSpace msp1(H5Screate_simple(1, m1.data(), nullptr));
    int32_t v11 = 5555;
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, msp1.get(), sp10.get(),
                       dxpl_off.get(), &v11),
              0);
    hs = {8, 8};
    ASSERT_GE(H5Sselect_hyperslab(sp10.get(), H5S_SELECT_SET, hs.data(),
                                  nullptr, c1x1.data(), nullptr),
              0);
    int32_t v22 = 8888;
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, msp1.get(), sp10.get(),
                       dxpl_off.get(), &v22),
              0);

    // Shrink to {6, 6}.
    std::array<hsize_t, 2> dims66 = {6, 6};
    ASSERT_GE(H5Dset_extent(dset.get(), dims66.data()), 0);
    ASSERT_GE(H5Dflush(dset.get()), 0);

    // Verify chunk {1, 1} became an unfiltered partial edge chunk (64 bytes).
    ASSERT_GE(
        H5Dget_chunk_info_by_coord(dset.get(), c11.data(), &fmask, nullptr, &csz),
        0);
    EXPECT_EQ(csz, 64);

    // Re-extend back to {8, 8} and read: pruned region must have fill value 77!
    std::array<hsize_t, 2> dims88 = {8, 8};
    ASSERT_GE(H5Dset_extent(dset.get(), dims88.data()), 0);
    std::vector<int32_t> buf88(64, -1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      dxpl_off.get(), buf88.data()),
              0);
    EXPECT_EQ(buf88[4 * 8 + 4], 5555);
    EXPECT_EQ(buf88[6 * 8 + 6], 77);
    EXPECT_EQ(buf88[7 * 8 + 7], 77);

    // Finally shrink down to {0, 0} to hit total chunk removal on shrink.
    std::array<hsize_t, 2> dims00 = {0, 0};
    ASSERT_GE(H5Dset_extent(dset.get(), dims00.data()), 0);
    hsize_t nchk = 99;
    ScopedSpace sp00(H5Dget_space(dset.get()));
    ASSERT_GE(H5Dget_num_chunks(dset.get(), sp00.get(), &nchk), 0);
    EXPECT_EQ(nchk, 0);
  }

  // 2. ALLOC_TIME_EARLY + FILL_TIME_NEVER + no filters (`should_fill = false`).
  {
    std::array<hsize_t, 1> dims = {8};
    std::array<hsize_t, 1> max_dims = {H5S_UNLIMITED};
    std::array<hsize_t, 1> cdims = {4};
    ScopedSpace space(H5Screate_simple(1, dims.data(), max_dims.data()));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_EARLY), 0);
    ASSERT_GE(H5Pset_fill_time(dcpl.get(), H5D_FILL_TIME_NEVER), 0);
    ScopedDataset d_early_nofill(H5Dcreate2(file.get(), "early_nofill",
                                            H5T_NATIVE_INT32, space.get(),
                                            H5P_DEFAULT, dcpl.get(),
                                            H5P_DEFAULT));
    ASSERT_TRUE(d_early_nofill.valid());
    dims = {16};
    ASSERT_GE(H5Dset_extent(d_early_nofill.get(), dims.data()), 0);
    hsize_t nchk = 0;
    ScopedSpace sp16(H5Dget_space(d_early_nofill.get()));
    ASSERT_GE(H5Dget_num_chunks(d_early_nofill.get(), sp16.get(), &nchk), 0);
    EXPECT_EQ(nchk, 4);
  }

  // 3. VL string fill value with filtered ALLOC_TIME_EARLY create + extend +
  // shrink/prune (hits has_vlen_fill_type in allocate & prune_fill!).
  {
    ScopedType vl_str(H5Tcopy(H5T_C_S1));
    ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);
    const char* vl_fill = "vl_init";
    std::array<hsize_t, 1> dims = {6};
    std::array<hsize_t, 1> max_dims = {H5S_UNLIMITED};
    std::array<hsize_t, 1> cdims = {4};
    ScopedSpace space(H5Screate_simple(1, dims.data(), max_dims.data()));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_EARLY), 0);
    ASSERT_GE(H5Pset_fill_value(dcpl.get(), vl_str.get(), &vl_fill), 0);

    ScopedDataset d_vl(H5Dcreate2(file.get(), "vl_early", vl_str.get(),
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(d_vl.valid());
    dims = {10};
    ASSERT_GE(H5Dset_extent(d_vl.get(), dims.data()), 0);
    dims = {2};
    ASSERT_GE(H5Dset_extent(d_vl.get(), dims.data()), 0);

    ScopedSpace sp2(H5Dget_space(d_vl.get()));
    std::array<char*, 2> out = {nullptr, nullptr};
    ASSERT_GE(H5Dread(d_vl.get(), vl_str.get(), H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, out.data()),
              0);
    auto reclaim = absl::MakeCleanup([&]() {
      H5Treclaim(vl_str.get(), sp2.get(), H5P_DEFAULT, out.data());
    });
    ASSERT_NE(out[0], nullptr);
    EXPECT_STREQ(out[0], "vl_init");
  }

  // 3b. VL string + deflate + DONT_FILTER_PARTIAL_CHUNKS + ALLOC_TIME_EARLY:
  // edge chunk skips filter pipeline (H5Dchunk.c:5518).
  {
    ScopedType vl_str(H5Tcopy(H5T_C_S1));
    ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);
    const char* vl_fill = "vl_edge";
    std::array<hsize_t, 1> dims = {6};
    std::array<hsize_t, 1> cdims = {4};
    ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);
    ASSERT_GE(H5Pset_chunk_opts(dcpl.get(), H5D_CHUNK_DONT_FILTER_PARTIAL_CHUNKS),
              0);
    ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_EARLY), 0);
    ASSERT_GE(H5Pset_fill_value(dcpl.get(), vl_str.get(), &vl_fill), 0);

    ScopedDataset d_vl_edge(H5Dcreate2(file.get(), "vl_early_edge",
                                       vl_str.get(), space.get(), H5P_DEFAULT,
                                       dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(d_vl_edge.valid());

    std::array<char*, 6> out{};
    ASSERT_GE(H5Dread(d_vl_edge.get(), vl_str.get(), H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, out.data()),
              0);
    auto reclaim = absl::MakeCleanup([&]() {
      H5Treclaim(vl_str.get(), space.get(), H5P_DEFAULT, out.data());
    });
    EXPECT_STREQ(out[5], "vl_edge");
  }

  // 4. Edge-chunk extend & shrink corner cases:
  //   - {4, 5} -> {8, 8} (dim 0 chunk-aligned, H5Dchunk.c:5762)
  //   - {5, 5} -> {8, 5} (only dim 0 becomes full, !new_full_dim[1]: 5774)
  //   - {3, 3} -> {4, 4} (old_edge_chunk_sc[0] == 0 break: 5829)
  //   - {8, 6} -> {6, 6} (dim-0-only shrink + shared partial dim 1: 6437, 6456,
  //     6493-6494)
  //   - ALLOC_TIME_INCR sparse shrink {8, 8} -> {6, 6} (6099-6100)
  {
    std::array<hsize_t, 2> max_dims = {H5S_UNLIMITED, H5S_UNLIMITED};
    std::array<hsize_t, 2> cdims = {4, 4};
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);
    ASSERT_GE(H5Pset_chunk_opts(dcpl.get(), H5D_CHUNK_DONT_FILTER_PARTIAL_CHUNKS),
              0);
    int32_t fill_v = 11;
    ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_INT32, &fill_v), 0);

    // 4a. {4, 5} -> {8, 8}
    std::array<hsize_t, 2> d45 = {4, 5};
    ScopedSpace sp45(H5Screate_simple(2, d45.data(), max_dims.data()));
    ScopedDataset ds45(H5Dcreate2(file.get(), "ext45", H5T_NATIVE_INT32,
                                  sp45.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(ds45.valid());
    std::vector<int32_t> w45(20, 45);
    ASSERT_GE(H5Dwrite(ds45.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       dxpl_off.get(), w45.data()),
              0);
    std::array<hsize_t, 2> d88 = {8, 8};
    ASSERT_GE(H5Dset_extent(ds45.get(), d88.data()), 0);

    // 4b. {5, 5} -> {8, 5}
    std::array<hsize_t, 2> d55 = {5, 5};
    ScopedSpace sp55(H5Screate_simple(2, d55.data(), max_dims.data()));
    ScopedDataset ds55(H5Dcreate2(file.get(), "ext55", H5T_NATIVE_INT32,
                                  sp55.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(ds55.valid());
    std::vector<int32_t> w55(25, 55);
    ASSERT_GE(H5Dwrite(ds55.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       dxpl_off.get(), w55.data()),
              0);
    std::array<hsize_t, 2> d85 = {8, 5};
    ASSERT_GE(H5Dset_extent(ds55.get(), d85.data()), 0);

    // 4c. {3, 3} -> {4, 4}
    std::array<hsize_t, 2> d33 = {3, 3};
    ScopedSpace sp33(H5Screate_simple(2, d33.data(), max_dims.data()));
    ScopedDataset ds33(H5Dcreate2(file.get(), "ext33", H5T_NATIVE_INT32,
                                  sp33.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(ds33.valid());
    std::vector<int32_t> w33(9, 33);
    ASSERT_GE(H5Dwrite(ds33.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       dxpl_off.get(), w33.data()),
              0);
    std::array<hsize_t, 2> d44 = {4, 4};
    ASSERT_GE(H5Dset_extent(ds33.get(), d44.data()), 0);

    // 4d. {8, 6} -> {6, 6} (only dim 0 shrinks; chunk {1, 1} remains partial
    // due to dim 1).
    std::array<hsize_t, 2> d86 = {8, 6};
    ScopedSpace sp86(H5Screate_simple(2, d86.data(), max_dims.data()));
    ScopedDataset ds86(H5Dcreate2(file.get(), "shk86", H5T_NATIVE_INT32,
                                  sp86.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(ds86.valid());
    std::vector<int32_t> w86(48, 86);
    ASSERT_GE(H5Dwrite(ds86.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       dxpl_off.get(), w86.data()),
              0);
    ASSERT_GE(H5Dflush(ds86.get()), 0);
    std::array<hsize_t, 2> d66 = {6, 6};
    ASSERT_GE(H5Dset_extent(ds86.get(), d66.data()), 0);
    std::vector<int32_t> r66(36, -1);
    ASSERT_GE(H5Dread(ds86.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      dxpl_off.get(), r66.data()),
              0);
    EXPECT_EQ(r66[35], 86);

    // 4e. ALLOC_TIME_INCR sparse shrink {8, 8} -> {6, 6} with {1, 0} & {1, 1}
    // unallocated (H5Dchunk.c:6099-6100).
    ScopedSpace sp88(H5Screate_simple(2, d88.data(), max_dims.data()));
    ScopedDataset ds_sp(H5Dcreate2(file.get(), "shk_sparse", H5T_NATIVE_INT32,
                                   sp88.get(), H5P_DEFAULT, dcpl.get(),
                                   H5P_DEFAULT));
    ASSERT_TRUE(ds_sp.valid());
    std::array<hsize_t, 2> hs = {0, 0};
    ASSERT_GE(H5Sselect_hyperslab(sp88.get(), H5S_SELECT_SET, hs.data(),
                                  nullptr, d44.data(), nullptr),
              0);
    ScopedSpace m44(H5Screate_simple(2, d44.data(), nullptr));
    std::vector<int32_t> w16(16, 17);
    ASSERT_GE(H5Dwrite(ds_sp.get(), H5T_NATIVE_INT32, m44.get(), sp88.get(),
                       dxpl_off.get(), w16.data()),
              0);
    ASSERT_GE(H5Dflush(ds_sp.get()), 0);
    ASSERT_GE(H5Dset_extent(ds_sp.get(), d66.data()), 0);

    // 4f. Sub-chunk extend {2, 2} -> {3, 3} with DONT_FILTER_PARTIAL_CHUNKS
    //     hits H5D__chunk_update_old_edge_chunks early exit (5724);
    //     zero-dim shrink {8, 4} -> {8, 0} -> {2, 0} hits prune old_dim == 0
    //     (6326-6328) and H5D__chunk_allocate space_dim == 0 (5264-5266).
    std::array<hsize_t, 2> d22 = {2, 2};
    ScopedSpace sp22(H5Screate_simple(2, d22.data(), max_dims.data()));
    ScopedDataset ds22(H5Dcreate2(file.get(), "ext22_sub", H5T_NATIVE_INT32,
                                  sp22.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(ds22.valid());
    ASSERT_GE(H5Dset_extent(ds22.get(), d33.data()), 0);

    ScopedPlist dcpl_early_zero = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_alloc_time(dcpl_early_zero.get(), H5D_ALLOC_TIME_EARLY), 0);
    std::array<hsize_t, 2> d84 = {8, 4};
    ScopedSpace sp84(H5Screate_simple(2, d84.data(), max_dims.data()));
    ScopedDataset ds_zero(
        H5Dcreate2(file.get(), "zero_dim_early", H5T_NATIVE_INT32, sp84.get(),
                   H5P_DEFAULT, dcpl_early_zero.get(), H5P_DEFAULT));
    ASSERT_TRUE(ds_zero.valid());
    std::array<hsize_t, 2> d80 = {8, 0};
    ASSERT_GE(H5Dset_extent(ds_zero.get(), d80.data()), 0);
    std::array<hsize_t, 2> d20 = {2, 0};
    ASSERT_GE(H5Dset_extent(ds_zero.get(), d20.data()), 0);
    {
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      H5D_t* dz = UnwrapDataset(ds_zero.get());
      ASSERT_NE(dz, nullptr);
      EXPECT_GE(H5D__chunk_allocate(dz, false, d80.data()), 0);
    }
  }
}

// ---------------------------------------------------------------------------
// 7. File Allocation (SWMR Write, Expanding Filter v4 Error, Filter Callback)
// ---------------------------------------------------------------------------

TEST(H5DchunkTest, SwmrFileAllocAndCustomFilterErrorAndV4EncodeLimit) {
  auto cleanup_filter = absl::MakeCleanup([]() {
    if (H5Zfilter_avail(kCustomTestFilterId) > 0) {
      H5Zunregister(kCustomTestFilterId);
    }
  });
  RegisterCustomTestFilterOrFail(kCustomTestFilterId);

  // 1. SWMR write mode on POSIX TempFile keeps old chunk space when compressed
  // chunk size changes (avoids H5MF_xfree in H5D__chunk_file_alloc).
  {
    TempFile swmr_path("h5dchunk_swmr.h5");
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST,
                                   H5F_LIBVER_LATEST),
              0);
    ScopedFile f(
        H5Fcreate(swmr_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());

    std::array<hsize_t, 1> dims = {16};
    std::array<hsize_t, 1> max_dims = {H5S_UNLIMITED};
    std::array<hsize_t, 1> cdims = {8};
    ScopedSpace sp(H5Screate_simple(1, dims.data(), max_dims.data()));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);

    ScopedDataset ds(H5Dcreate2(f.get(), "swmr_ds", H5T_NATIVE_INT32, sp.get(),
                                H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    std::vector<int32_t> zeros(16, 0);
    ASSERT_GE(H5Dwrite(ds.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, zeros.data()),
              0);
    ASSERT_GE(H5Dflush(ds.get()), 0);
    ASSERT_GE(H5Fstart_swmr_write(f.get()), 0);

    // Write pseudo-varied data so compressed chunk size changes under SWMR write.
    std::vector<int32_t> varied(16);
    for (int i = 0; i < 16; ++i) {
      varied[i] = i * 7919 + 13;
    }
    ASSERT_GE(H5Dwrite(ds.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, varied.data()),
              0);
    ASSERT_GE(H5Dflush(ds.get()), 0);

    std::vector<int32_t> readback(16, -1);
    ASSERT_GE(H5Dread(ds.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, readback.data()),
              0);
    EXPECT_EQ(readback, varied);
  }

  // 2. Expanding filter hitting H5D_CHUNK_ENCODE_SIZE_CHECK in layout v4
  // (V110..V114), while succeeding in layout v5 (V200).
  {
    std::array<hsize_t, 1> dims = {50};
    std::array<hsize_t, 1> cdims = {25};  // 100 bytes/chunk -> v4 limit 65535 B.
    ScopedSpace sp(H5Screate_simple(1, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    const std::array<unsigned int, 2> cd_expand = {1u, 70000u};
    ASSERT_GE(H5Pset_filter(dcpl.get(), kCustomTestFilterId,
                            H5Z_FLAG_MANDATORY, cd_expand.size(),
                            cd_expand.data()),
              0);

    // V110..V114 keeps layout version 4 with Fixed Array index -> fails flush!
    {
      ScopedErrorSilence silence;
      ScopedPlist fapl_v4 =
          MakeFaplWithLibver(H5F_LIBVER_V110, H5F_LIBVER_V114);
      ScopedFile fv4(
          H5Fcreate("v4_expand.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v4.get()));
      ASSERT_TRUE(fv4.valid());
      ScopedDataset d4(H5Dcreate2(fv4.get(), "exp", H5T_NATIVE_INT32, sp.get(),
                                  H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
      ASSERT_TRUE(d4.valid());
      std::vector<int32_t> data(50, 42);
      // H5Dwrite caches chunks; H5Dflush triggers H5D__chunk_file_alloc failure!
      H5Dwrite(d4.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
               data.data());
      EXPECT_LT(H5Dflush(d4.get()), 0);
      // Explicit H5Dclose also fails while flushing dirty cache entry; release
      // via reset after silence.
      d4.reset();
    }

    // V200 uses layout version 5 -> succeeds!
    {
      ScopedPlist fapl_v5 =
          MakeFaplWithLibver(H5F_LIBVER_V200, H5F_LIBVER_V200);
      ScopedFile fv5(
          H5Fcreate("v5_expand.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v5.get()));
      ASSERT_TRUE(fv5.valid());
      ScopedDataset d5(H5Dcreate2(fv5.get(), "exp", H5T_NATIVE_INT32, sp.get(),
                                  H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
      ASSERT_TRUE(d5.valid());
      std::vector<int32_t> data(50);
      std::iota(data.begin(), data.end(), 100);
      ASSERT_GE(H5Dwrite(d5.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                         H5P_DEFAULT, data.data()),
                0);
      ASSERT_GE(H5Dflush(d5.get()), 0);
      std::vector<int32_t> out(50, -1);
      ASSERT_GE(H5Dread(d5.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                        H5P_DEFAULT, out.data()),
                0);
      EXPECT_EQ(out, data);
    }
  }
}

// ---------------------------------------------------------------------------
// 8. Object Copy (Cached Chunks, VLEN, References, Edge Chunks), BH Info,
//    Format Convert, and Debug Dump
// ---------------------------------------------------------------------------

TEST(H5DchunkTest, CopyBhInfoFormatConvertAndDump) {
  ScopedFile src_file = CreateInMemoryFile("copy_src.h5");
  ScopedFile dst_file = CreateInMemoryFile("copy_dst.h5");
  ASSERT_TRUE(src_file.valid());
  ASSERT_TRUE(dst_file.valid());

  ScopedPlist dxpl_off(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(H5Pset_selection_io(dxpl_off.get(), H5D_SELECTION_IO_MODE_OFF), 0);

  // 1. Copy filtered dataset with DONT_FILTER_PARTIAL_CHUNKS while BOTH:
  //    - an allocated chunk {0, 0} is dirty in RDCC cache (chunk_in_cache), AND
  //    - an un-allocated chunk {4, 0} is dirty ONLY in RDCC cache
  //      (shared_fo->cache.chunk.head loop with HADDR_UNDEF!).
  {
    std::array<hsize_t, 2> dims = {6, 5};
    std::array<hsize_t, 2> cdims = {4, 4};
    ScopedSpace sp(H5Screate_simple(2, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);
    ASSERT_GE(H5Pset_chunk_opts(dcpl.get(), H5D_CHUNK_DONT_FILTER_PARTIAL_CHUNKS),
              0);

    ScopedDataset src_ds(H5Dcreate2(src_file.get(), "cached_copy",
                                    H5T_NATIVE_INT32, sp.get(), H5P_DEFAULT,
                                    dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(src_ds.valid());

    // Write & flush chunk {0, 0} first so it has a disk address.
    std::array<hsize_t, 2> hs = {0, 0};
    std::array<hsize_t, 2> cnt = {4, 4};
    ASSERT_GE(H5Sselect_hyperslab(sp.get(), H5S_SELECT_SET, hs.data(), nullptr,
                                  cnt.data(), nullptr),
              0);
    ScopedSpace m44(H5Screate_simple(2, cnt.data(), nullptr));
    std::vector<int32_t> vals16(16, 10);
    ASSERT_GE(H5Dwrite(src_ds.get(), H5T_NATIVE_INT32, m44.get(), sp.get(),
                       dxpl_off.get(), vals16.data()),
              0);
    ASSERT_GE(H5Dflush(src_ds.get()), 0);

    // Now dirty chunk {0, 0} again in cache (keeps defined disk addr).
    std::array<hsize_t, 2> c11 = {1, 1};
    ASSERT_GE(H5Sselect_hyperslab(sp.get(), H5S_SELECT_SET, hs.data(), nullptr,
                                  c11.data(), nullptr),
              0);
    ScopedSpace m11(H5Screate_simple(2, c11.data(), nullptr));
    int32_t updated00 = 99;
    ASSERT_GE(H5Dwrite(src_ds.get(), H5T_NATIVE_INT32, m11.get(), sp.get(),
                       dxpl_off.get(), &updated00),
              0);

    // Dirty chunk {4, 0} ONLY in cache (never flushed -> HADDR_UNDEF).
    hs = {4, 0};
    ASSERT_GE(H5Sselect_hyperslab(sp.get(), H5S_SELECT_SET, hs.data(), nullptr,
                                  c11.data(), nullptr),
              0);
    int32_t cached40 = 404;
    ASSERT_GE(H5Dwrite(src_ds.get(), H5T_NATIVE_INT32, m11.get(), sp.get(),
                       dxpl_off.get(), &cached40),
              0);

    // Copy while src_ds stays open!
    ASSERT_GE(H5Ocopy(src_file.get(), "cached_copy", dst_file.get(),
                      "copied_cached", H5P_DEFAULT, H5P_DEFAULT),
              0);

    ScopedDataset dst_ds(H5Dopen2(dst_file.get(), "copied_cached", H5P_DEFAULT));
    ASSERT_TRUE(dst_ds.valid());
    std::vector<int32_t> out(30, -1);
    ASSERT_GE(H5Dread(dst_ds.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      dxpl_off.get(), out.data()),
              0);
    EXPECT_EQ(out[0], 99);
    EXPECT_EQ(out[1], 10);
    EXPECT_EQ(out[4 * 5 + 0], 404);

    // Query H5Oget_native_info (exercises H5D__chunk_bh_info on filtered
    // dataset via H5O_NATIVE_INFO_META_SIZE).
    H5O_native_info_t ninfo{};
    ASSERT_GE(H5Oget_native_info(dst_ds.get(), &ninfo,
                                 H5O_NATIVE_INFO_HDR |
                                     H5O_NATIVE_INFO_META_SIZE),
              0);
    EXPECT_GT(ninfo.meta_size.obj.index_size, 0);
  }

  // 1b. Copy and query bh_info on an UNFILTERED chunked dataset (7139-7141,
  // 7381), plus open-dataset H5Ocopy with rdcc_nslots = 1 where cache slot
  // holds a DIFFERENT chunk (6960-6961).
  {
    std::array<hsize_t, 1> dims = {8};
    std::array<hsize_t, 1> cdims = {4};
    ScopedSpace sp(H5Screate_simple(1, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ScopedPlist dapl_1slot(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_chunk_cache(dapl_1slot.get(), 1, 4096, 0.5), 0);

    ScopedDataset src_unfilt(H5Dcreate2(src_file.get(), "unfilt_copy",
                                        H5T_NATIVE_INT32, sp.get(), H5P_DEFAULT,
                                        dcpl.get(), dapl_1slot.get()));
    ASSERT_TRUE(src_unfilt.valid());
    std::array<hsize_t, 1> hs = {0};
    std::array<hsize_t, 1> cnt = {4};
    ASSERT_GE(H5Sselect_hyperslab(sp.get(), H5S_SELECT_SET, hs.data(), nullptr,
                                  cnt.data(), nullptr),
              0);
    ScopedSpace m4(H5Screate_simple(1, cnt.data(), nullptr));
    std::array<int32_t, 4> c0 = {10, 20, 30, 40};
    ASSERT_GE(H5Dwrite(src_unfilt.get(), H5T_NATIVE_INT32, m4.get(), sp.get(),
                       dxpl_off.get(), c0.data()),
              0);
    ASSERT_GE(H5Dflush(src_unfilt.get()), 0);

    // Writing chunk {4} preempts {0} so slot 0 now holds chunk {1} dirty!
    hs = {4};
    ASSERT_GE(H5Sselect_hyperslab(sp.get(), H5S_SELECT_SET, hs.data(), nullptr,
                                  cnt.data(), nullptr),
              0);
    std::array<int32_t, 4> c1 = {50, 60, 70, 80};
    ASSERT_GE(H5Dwrite(src_unfilt.get(), H5T_NATIVE_INT32, m4.get(), sp.get(),
                       dxpl_off.get(), c1.data()),
              0);

    // Keep src_unfilt open while copying: chunk 0 hashes to slot 0 which holds
    // chunk 1 -> H5Dchunk.c:6960-6961!
    ASSERT_GE(H5Ocopy(src_file.get(), "unfilt_copy", dst_file.get(),
                      "unfilt_copy_dst", H5P_DEFAULT, H5P_DEFAULT),
              0);

    ScopedDataset dst_unfilt(
        H5Dopen2(dst_file.get(), "unfilt_copy_dst", H5P_DEFAULT));
    ASSERT_TRUE(dst_unfilt.valid());
    std::array<int32_t, 8> out_unfilt{};
    ASSERT_GE(H5Dread(dst_unfilt.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      dxpl_off.get(), out_unfilt.data()),
              0);
    EXPECT_THAT(out_unfilt, ElementsAre(10, 20, 30, 40, 50, 60, 70, 80));

    H5O_native_info_t ninfo{};
    ASSERT_GE(H5Oget_native_info(dst_unfilt.get(), &ninfo,
                                 H5O_NATIVE_INFO_HDR |
                                     H5O_NATIVE_INFO_META_SIZE),
              0);
    EXPECT_GT(ninfo.meta_size.obj.index_size, 0);
  }

  // 2. Copy filtered VLEN chunked dataset across files (hits VLEN convert,
  // reclaim, and pipeline re-compression).
  {
    ScopedType vl_str(H5Tcopy(H5T_C_S1));
    ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);
    const char* fill_s = "fill";
    std::array<hsize_t, 1> dims = {6};
    std::array<hsize_t, 1> cdims = {3};
    ScopedSpace sp(H5Screate_simple(1, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);
    ASSERT_GE(H5Pset_fill_value(dcpl.get(), vl_str.get(), &fill_s), 0);

    ScopedDataset src_vl(H5Dcreate2(src_file.get(), "vl_copy", vl_str.get(),
                                    sp.get(), H5P_DEFAULT, dcpl.get(),
                                    H5P_DEFAULT));
    ASSERT_TRUE(src_vl.valid());
    const std::array<const char*, 6> strs = {"alpha", "beta", "gamma",
                                             "delta", "epsilon", "zeta"};
    ASSERT_GE(H5Dwrite(src_vl.get(), vl_str.get(), H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, strs.data()),
              0);
    src_vl.reset();

    ASSERT_GE(H5Ocopy(src_file.get(), "vl_copy", dst_file.get(), "vl_copy_dst",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);

    ScopedDataset dst_vl(H5Dopen2(dst_file.get(), "vl_copy_dst", H5P_DEFAULT));
    ASSERT_TRUE(dst_vl.valid());
    std::array<char*, 6> rstrs{};
    ASSERT_GE(H5Dread(dst_vl.get(), vl_str.get(), H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, rstrs.data()),
              0);
    auto reclaim = absl::MakeCleanup([&]() {
      H5Treclaim(vl_str.get(), sp.get(), H5P_DEFAULT, rstrs.data());
    });
    EXPECT_STREQ(rstrs[0], "alpha");
    EXPECT_STREQ(rstrs[5], "zeta");
  }

  // 3. Copy object-reference chunked dataset with EXPAND_REFERENCE_FLAG ON &
  // OFF.
  {
    std::array<hsize_t, 1> tdims = {2};
    ScopedSpace tsp(H5Screate_simple(1, tdims.data(), nullptr));
    ScopedDataset target(H5Dcreate2(src_file.get(), "ref_target",
                                    H5T_NATIVE_INT32, tsp.get(), H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT));
    std::array<int32_t, 2> tv = {123, 456};
    ASSERT_GE(H5Dwrite(target.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, tv.data()),
              0);
    target.reset();

    hobj_ref_t ref{};
    ASSERT_GE(
        H5Rcreate(&ref, src_file.get(), "/ref_target", H5R_OBJECT, -1), 0);

    std::array<hsize_t, 1> rdims = {2};
    std::array<hsize_t, 1> rcdims = {2};
    ScopedSpace rsp(H5Screate_simple(1, rdims.data(), nullptr));
    ScopedPlist dcpl_ref = MakeChunkedDcpl(rcdims);
    ASSERT_GE(H5Pset_deflate(dcpl_ref.get(), 6), 0);
    ScopedDataset src_ref(H5Dcreate2(src_file.get(), "ref_ds", H5T_STD_REF_OBJ,
                                     rsp.get(), H5P_DEFAULT, dcpl_ref.get(),
                                     H5P_DEFAULT));
    std::array<hobj_ref_t, 2> refs = {ref, ref};
    ASSERT_GE(H5Dwrite(src_ref.get(), H5T_STD_REF_OBJ, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, refs.data()),
              0);
    src_ref.reset();

    // Without expand flag: destination refs zeroed out.
    ASSERT_GE(H5Ocopy(src_file.get(), "ref_ds", dst_file.get(),
                      "ref_ds_no_expand", H5P_DEFAULT, H5P_DEFAULT),
              0);
    // With expand flag: copies target object and updates reference!
    ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
    ASSERT_GE(H5Pset_copy_object(ocpypl.get(), H5O_COPY_EXPAND_REFERENCE_FLAG),
              0);
    ASSERT_GE(H5Ocopy(src_file.get(), "ref_ds", dst_file.get(),
                      "ref_ds_expand", ocpypl.get(), H5P_DEFAULT),
              0);

    ScopedDataset dst_ref(
        H5Dopen2(dst_file.get(), "ref_ds_expand", H5P_DEFAULT));
    std::array<hobj_ref_t, 2> out_refs{};
    ASSERT_GE(H5Dread(dst_ref.get(), H5T_STD_REF_OBJ, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, out_refs.data()),
              0);
    ScopedObject resolved(
        H5Rdereference2(dst_ref.get(), H5P_DEFAULT, H5R_OBJECT, &out_refs[0]));
    ASSERT_TRUE(resolved.valid());

    // 1-element H5T_STD_REF_OBJ + deflate has nbytes > 8 (= buf_size), forcing
    // udata->buf and udata->bkg reallocation in H5D__chunk_copy_cb (6912-6933),
    // and same-file H5Ocopy hits the non-convertible do_convert error (6908).
    std::array<hsize_t, 1> dims1 = {1};
    ScopedSpace sp1(H5Screate_simple(1, dims1.data(), nullptr));
    ScopedPlist dcpl_ref1 = MakeChunkedDcpl(dims1);
    ASSERT_GE(H5Pset_deflate(dcpl_ref1.get(), 6), 0);
    ScopedDataset src_ref1(H5Dcreate2(src_file.get(), "ref_ds1", H5T_STD_REF_OBJ,
                                      sp1.get(), H5P_DEFAULT, dcpl_ref1.get(),
                                      H5P_DEFAULT));
    ASSERT_TRUE(src_ref1.valid());
    ASSERT_GE(H5Dwrite(src_ref1.get(), H5T_STD_REF_OBJ, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, &ref),
              0);
    src_ref1.reset();

    ASSERT_GE(H5Ocopy(src_file.get(), "ref_ds1", dst_file.get(),
                      "ref_ds1_no_expand", H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Ocopy(src_file.get(), "ref_ds1", dst_file.get(),
                      "ref_ds1_expand", ocpypl.get(), H5P_DEFAULT),
              0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Ocopy(src_file.get(), "ref_ds1", src_file.get(),
                        "ref_ds1_same_file", H5P_DEFAULT, H5P_DEFAULT),
                0);
    }
  }

  // 4. H5Dformat_convert (with DONT_FILTER_PARTIAL_CHUNKS) & H5D__chunk_dump_index.
  {
    TempFile tmp_conv("fmt_conv.h5");
    ScopedPlist fapl_v110(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl_v110.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl_v110.get(), H5F_LIBVER_V110, H5F_LIBVER_V114),
        0);
    ScopedFile fconv(
        H5Fcreate(tmp_conv.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v110.get()));
    ASSERT_TRUE(fconv.valid());

    std::array<hsize_t, 2> dims = {6, 5};
    std::array<hsize_t, 2> cdims = {4, 4};
    ScopedSpace sp(H5Screate_simple(2, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);
    ASSERT_GE(H5Pset_chunk_opts(dcpl.get(), H5D_CHUNK_DONT_FILTER_PARTIAL_CHUNKS),
              0);

    ScopedDataset ds(H5Dcreate2(fconv.get(), "farray_edge", H5T_NATIVE_INT32,
                                sp.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
    std::vector<int32_t> vals(30);
    std::iota(vals.begin(), vals.end(), 10);
    ASSERT_GE(H5Dwrite(ds.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, vals.data()),
              0);
    ASSERT_GE(H5Dflush(ds.get()), 0);

    H5D_chunk_index_t idx_before = H5D_CHUNK_IDX_NTYPES;
    ASSERT_GE(H5Dget_chunk_index_type(ds.get(), &idx_before), 0);
    EXPECT_EQ(idx_before, H5D_CHUNK_IDX_FARRAY);

    ASSERT_GE(H5Dformat_convert(ds.get()), 0);

    // Dump index into a TempFile FILE* (and test NULL stream) before closing.
    TempFile dump_file("chunk_dump.txt");
    FILE* stream = std::fopen(dump_file.c_str(), "w+");
    ASSERT_NE(stream, nullptr);
    auto close_stream = absl::MakeCleanup([&]() { std::fclose(stream); });
    {
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      H5D_t* d = UnwrapDataset(ds.get());
      ASSERT_NE(d, nullptr);
      EXPECT_EQ(d->shared->layout.storage.u.chunk.idx_type,
                H5D_CHUNK_IDX_BTREE);
      ASSERT_GE(H5D__chunk_dump_index(d, nullptr), 0);
      ASSERT_GE(H5D__chunk_dump_index(d, stream), 0);
    }
    std::fflush(stream);
    std::rewind(stream);
    std::string dump_text(2048, '\0');
    const size_t nread = std::fread(dump_text.data(), 1, dump_text.size(), stream);
    dump_text.resize(nread);
    EXPECT_THAT(dump_text, HasSubstr("Logical Offset"));
    EXPECT_THAT(dump_text, HasSubstr("Address"));
    EXPECT_THAT(dump_text, HasSubstr("[0, 0, 0]"));

    // Close and reopen the file so the v3 layout message (which clears v4
    // DONT_FILTER_PARTIAL_CHUNKS flags and resets u.chunk.idx_type to BTREE)
    // is decoded fresh from disk, then verify readback.
    ds.reset();
    fconv.reset();
    fconv.reset(H5Fopen(tmp_conv.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(fconv.valid());
    ds.reset(H5Dopen2(fconv.get(), "farray_edge", H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());

    H5D_chunk_index_t idx_after = H5D_CHUNK_IDX_NTYPES;
    ASSERT_GE(H5Dget_chunk_index_type(ds.get(), &idx_after), 0);
    EXPECT_EQ(idx_after, H5D_CHUNK_IDX_BTREE);

    std::vector<int32_t> out(30, -1);
    ASSERT_GE(H5Dread(ds.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, out.data()),
              0);
    EXPECT_EQ(out, vals);
  }
}

// ---------------------------------------------------------------------------
// 9. Internal Helper Routines & Link Deletion
// ---------------------------------------------------------------------------

TEST(H5DchunkTest, InternalHelpersAndDatasetDelete) {
  ScopedFile file = CreateInMemoryFile("helpers.h5");
  ASSERT_TRUE(file.valid());

  std::array<hsize_t, 2> dims = {6, 8};
  std::array<hsize_t, 2> cdims = {4, 4};
  ScopedSpace sp(H5Screate_simple(2, dims.data(), nullptr));
  ScopedPlist dcpl = MakeChunkedDcpl(cdims);
  ScopedDataset ds(H5Dcreate2(file.get(), "ds", H5T_NATIVE_INT32, sp.get(),
                              H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(ds.valid());

  {
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    H5D_t* d = UnwrapDataset(ds.get());
    ASSERT_NE(d, nullptr);
    EXPECT_FALSE(H5D__chunk_is_space_alloc(&d->shared->layout.storage));
    EXPECT_FALSE(H5D__chunk_is_data_cached(d->shared));
  }

  std::vector<int32_t> vals(48, 5);
  ASSERT_GE(H5Dwrite(ds.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     vals.data()),
            0);
  ASSERT_GE(H5Dflush(ds.get()), 0);

  std::array<hsize_t, 1> dims_f = {8};
  std::array<hsize_t, 1> cdims_f = {4};
  ScopedSpace sp_f(H5Screate_simple(1, dims_f.data(), nullptr));
  ScopedPlist dcpl_f = MakeChunkedDcpl(cdims_f);
  ASSERT_GE(H5Pset_deflate(dcpl_f.get(), 6), 0);
  ScopedDataset ds_f(H5Dcreate2(file.get(), "ds_filt_alloc", H5T_NATIVE_INT32,
                                sp_f.get(), H5P_DEFAULT, dcpl_f.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(ds_f.valid());
  std::array<int32_t, 8> vf = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_GE(H5Dwrite(ds_f.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, vf.data()),
            0);
  ASSERT_GE(H5Dflush(ds_f.get()), 0);

  {
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    H5D_t* d = UnwrapDataset(ds.get());
    ASSERT_NE(d, nullptr);

    // 1. H5D__chunk_is_space_alloc & H5D__chunk_is_data_cached (flush keeps
    // clean chunks in RDCC without evicting them).
    EXPECT_TRUE(H5D__chunk_is_space_alloc(&d->shared->layout.storage));
    EXPECT_TRUE(H5D__chunk_is_data_cached(d->shared));

    // 2. H5D__chunk_is_partial_edge_chunk.
    std::array<hsize_t, 2> sc00 = {0, 0};
    std::array<hsize_t, 2> sc10 = {1, 0};
    EXPECT_FALSE(H5D__chunk_is_partial_edge_chunk(2, cdims.data(), sc00.data(),
                                                  dims.data()));
    EXPECT_TRUE(H5D__chunk_is_partial_edge_chunk(2, cdims.data(), sc10.data(),
                                                 dims.data()));

    // 3. H5D__chunk_get_offset_copy valid, OOB, and misaligned.
    std::array<hsize_t, H5O_LAYOUT_NDIMS> off_copy{};
    std::array<hsize_t, 2> good_off = {4, 4};
    EXPECT_GE(H5D__chunk_get_offset_copy(d, good_off.data(), off_copy.data()),
              0);
    EXPECT_EQ(off_copy[0], 4);
    EXPECT_EQ(off_copy[1], 4);
    EXPECT_EQ(off_copy[2], 0);

    {
      ScopedErrorSilence silence;
      std::array<hsize_t, 2> oob = {8, 0};
      EXPECT_LT(H5D__chunk_get_offset_copy(d, oob.data(), off_copy.data()), 0);
      std::array<hsize_t, 2> unaligned = {2, 0};
      EXPECT_LT(
          H5D__chunk_get_offset_copy(d, unaligned.data(), off_copy.data()), 0);
    }

    // 4. H5D__chunk_lookup hitting H5D__chunk_cinfo_cache_found on second call.
    H5D_chunk_ud_t udata{};
    EXPECT_GE(H5D__chunk_lookup(d, sc00.data(), &udata), 0);
    const haddr_t addr0 = udata.chunk_block.offset;
    EXPECT_NE(addr0, HADDR_UNDEF);
    H5D_chunk_ud_t udata2{};
    EXPECT_GE(H5D__chunk_lookup(d, sc00.data(), &udata2), 0);
    EXPECT_EQ(udata2.chunk_block.offset, addr0);

    // 5. H5D__chunk_allocated.
    hsize_t alloc_bytes = 0;
    EXPECT_GE(H5D__chunk_allocated(d, &alloc_bytes), 0);
    EXPECT_GT(alloc_bytes, 0);

    // 5b. H5D__chunk_set_info error when ndims == 0 (740, 793), restored safely.
    {
      ScopedErrorSilence silence;
      const unsigned saved_ndims = d->shared->ndims;
      auto restore_ndims =
          absl::MakeCleanup([&]() { d->shared->ndims = saved_ndims; });
      d->shared->ndims = 0;
      EXPECT_LT(H5D__chunk_set_info(d), 0);
    }

    // 5c. H5D__chunk_file_alloc on a filtered flushed chunk with same length
    //     and HADDR_UNDEF new_chunk offset reuses old_chunk->offset (7761).
    {
      H5D_t* df = UnwrapDataset(ds_f.get());
      ASSERT_NE(df, nullptr);
      std::array<hsize_t, 1> sc0 = {0};
      H5D_chunk_ud_t udf{};
      ASSERT_GE(H5D__chunk_lookup(df, sc0.data(), &udf), 0);
      ASSERT_NE(udf.chunk_block.offset, HADDR_UNDEF);
      ASSERT_GT(udf.chunk_block.length, 0);

      H5D_chk_idx_info_t idx_info_f{df->oloc.file, &df->shared->dcpl_cache.pline,
                                    &df->shared->layout};
      const H5F_block_t old_blk = udf.chunk_block;
      H5F_block_t new_blk{HADDR_UNDEF, old_blk.length};
      bool need_insert = true;
      EXPECT_GE(H5D__chunk_file_alloc(&idx_info_f, &old_blk, &new_blk,
                                      &need_insert, sc0.data()),
                0);
      EXPECT_EQ(new_blk.offset, old_blk.offset);
      EXPECT_FALSE(need_insert);
    }

    // 6. H5D__chunk_mem_alloc / realloc / free / xfree (unfiltered & filtered).
    H5O_pline_t pl_empty{};
    void* mem_unfilt = H5D__chunk_mem_alloc(64, &pl_empty);
    ASSERT_NE(mem_unfilt, nullptr);
    mem_unfilt = H5D__chunk_mem_realloc(mem_unfilt, 128, &pl_empty);
    ASSERT_NE(mem_unfilt, nullptr);
    mem_unfilt = H5D__chunk_mem_xfree(mem_unfilt, &pl_empty);
    EXPECT_EQ(mem_unfilt, nullptr);
    void* mem_free = H5D__chunk_mem_alloc(32, &pl_empty);
    ASSERT_NE(mem_free, nullptr);
    H5D__chunk_mem_free(mem_free, &pl_empty);

    H5O_pline_t pl_filt{};
    pl_filt.nused = 1;
    void* mem_filt = H5D__chunk_mem_alloc(64, &pl_filt);
    ASSERT_NE(mem_filt, nullptr);
    mem_filt = H5D__chunk_mem_realloc(mem_filt, 128, &pl_filt);
    ASSERT_NE(mem_filt, nullptr);
    mem_filt = H5D__chunk_mem_xfree(mem_filt, &pl_filt);
    EXPECT_EQ(mem_filt, nullptr);
    void* mem_from_null = H5D__chunk_mem_realloc(nullptr, 48, &pl_filt);
    ASSERT_NE(mem_from_null, nullptr);
    H5D__chunk_mem_free(mem_from_null, &pl_filt);
    EXPECT_EQ(H5D__chunk_mem_xfree(nullptr, &pl_filt), nullptr);
  }

  // Delete the dataset via H5Ldelete and close it so H5D__chunk_delete frees
  // chunk storage & index cleanly.
  ds.reset();
  ASSERT_GE(H5Ldelete(file.get(), "ds", H5P_DEFAULT), 0);
}

TEST(H5DchunkTest, UncachedEdgeFiltersResizeFailureAndDelete) {
  auto cleanup_filter = absl::MakeCleanup([]() {
    if (H5Zfilter_avail(kCustomTestFilterId) > 0) {
      H5Zunregister(kCustomTestFilterId);
    }
  });
  RegisterCustomTestFilterOrFail(kCustomTestFilterId);

  ScopedFile file = CreateInMemoryFile("uncached_fail.h5");
  ASSERT_TRUE(file.valid());
  ScopedPlist dxpl_off(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(H5Pset_selection_io(dxpl_off.get(), H5D_SELECTION_IO_MODE_OFF), 0);

  // 1. Uncached chunk unlock (rdcc_nbytes = 0) with DONT_FILTER_PARTIAL_CHUNKS
  //    covering:
  //    - clean uncached read (5088-5090),
  //    - shrink turning full chunk into newly unfiltered edge chunk
  //      (5056-5060, 5074, 5076),
  //    - dirty/clean partial edge chunk via dxpl_off (5061-5066, 5074, 5088).
  {
    ScopedPlist dapl_nocache(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_chunk_cache(dapl_nocache.get(), 0, 0, 0.0), 0);

    std::array<hsize_t, 2> dims = {8, 8};
    std::array<hsize_t, 2> max_dims = {H5S_UNLIMITED, H5S_UNLIMITED};
    std::array<hsize_t, 2> cdims = {4, 4};
    ScopedSpace sp8(H5Screate_simple(2, dims.data(), max_dims.data()));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);
    ASSERT_GE(H5Pset_chunk_opts(dcpl.get(), H5D_CHUNK_DONT_FILTER_PARTIAL_CHUNKS),
              0);
    int32_t fill_v = -7;
    ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_INT32, &fill_v), 0);

    ScopedDataset ds8(H5Dcreate2(file.get(), "uncached_shrink",
                                 H5T_NATIVE_INT32, sp8.get(), H5P_DEFAULT,
                                 dcpl.get(), dapl_nocache.get()));
    ASSERT_TRUE(ds8.valid());
    std::vector<int32_t> vals(64, 42);
    ASSERT_GE(H5Dwrite(ds8.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       dxpl_off.get(), vals.data()),
              0);
    std::vector<int32_t> rvals(64, -1);
    ASSERT_GE(H5Dread(ds8.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      dxpl_off.get(), rvals.data()),
              0);
    EXPECT_EQ(rvals, vals);

    std::array<hsize_t, 2> dims6 = {6, 6};
    ASSERT_GE(H5Dset_extent(ds8.get(), dims6.data()), 0);
    std::vector<int32_t> r6(36, -1);
    ASSERT_GE(H5Dread(ds8.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      dxpl_off.get(), r6.data()),
              0);
    EXPECT_EQ(r6[35], 42);

    // Partial edge {5, 5} with rdcc_nbytes = 0: write then read partial corner
    // {4, 4} so dirty=true (5061-5066, 5074) and dirty=false (5088-5090,
    // is_unfiltered_edge_chunk=true) execute.
    std::array<hsize_t, 2> dims5 = {5, 5};
    ScopedSpace sp5(H5Screate_simple(2, dims5.data(), nullptr));
    ScopedDataset ds5(H5Dcreate2(file.get(), "uncached_edge5",
                                 H5T_NATIVE_INT32, sp5.get(), H5P_DEFAULT,
                                 dcpl.get(), dapl_nocache.get()));
    ASSERT_TRUE(ds5.valid());
    std::array<hsize_t, 2> hs = {4, 4};
    std::array<hsize_t, 2> cnt = {1, 1};
    ASSERT_GE(H5Sselect_hyperslab(sp5.get(), H5S_SELECT_SET, hs.data(), nullptr,
                                  cnt.data(), nullptr),
              0);
    ScopedSpace m1(H5Screate_simple(2, cnt.data(), nullptr));
    int32_t edge_elem = 1234;
    ASSERT_GE(H5Dwrite(ds5.get(), H5T_NATIVE_INT32, m1.get(), sp5.get(),
                       dxpl_off.get(), &edge_elem),
              0);
    int32_t edge_read = 0;
    ASSERT_GE(H5Dread(ds5.get(), H5T_NATIVE_INT32, m1.get(), sp5.get(),
                      dxpl_off.get(), &edge_read),
              0);
    EXPECT_EQ(edge_read, 1234);
  }

  // 1b. Cached ALLOC_TIME_INCR + FILL_TIME_NEVER partial chunk write zeroes
  //     unfilled chunk memory in H5D__chunk_lock (4911).
  {
    std::array<hsize_t, 1> dims = {4};
    std::array<hsize_t, 1> cdims = {4};
    ScopedSpace sp(H5Screate_simple(1, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_INCR), 0);
    ASSERT_GE(H5Pset_fill_time(dcpl.get(), H5D_FILL_TIME_NEVER), 0);
    ScopedPlist dapl_nf(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_chunk_cache(dapl_nf.get(), 13, 1024, 0.5), 0);
    ScopedDataset ds_nf(H5Dcreate2(file.get(), "cached_nofill",
                                   H5T_NATIVE_INT32, sp.get(), H5P_DEFAULT,
                                   dcpl.get(), dapl_nf.get()));
    ASSERT_TRUE(ds_nf.valid());

    std::array<hsize_t, 1> hs = {0};
    std::array<hsize_t, 1> cnt = {1};
    ASSERT_GE(H5Sselect_hyperslab(sp.get(), H5S_SELECT_SET, hs.data(), nullptr,
                                  cnt.data(), nullptr),
              0);
    ScopedSpace m1(H5Screate_simple(1, cnt.data(), nullptr));
    int32_t v = 77;
    ASSERT_GE(H5Dwrite(ds_nf.get(), H5T_NATIVE_INT32, m1.get(), sp.get(),
                       dxpl_off.get(), &v),
              0);
    int32_t rv = 0;
    ASSERT_GE(H5Dread(ds_nf.get(), H5T_NATIVE_INT32, m1.get(), sp.get(),
                      dxpl_off.get(), &rv),
              0);
    EXPECT_EQ(rv, 77);
  }

  // 2. VL string + ALLOC_TIME_EARLY + shrink filter (mode = 4): chunk 0
  //    shrinks fb_info.fill_buf_size below orig_chunk_size, triggering
  //    H5D__chunk_mem_realloc in H5D__chunk_allocate (5485-5491).
  {
    ScopedType vl_str(H5Tcopy(H5T_C_S1));
    ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);
    const char* vl_fill = "vl_shrink_fill";
    std::array<hsize_t, 1> dims = {16};
    std::array<hsize_t, 1> cdims = {8};
    ScopedSpace sp(H5Screate_simple(1, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    const unsigned int cd_shrink = 4;
    ASSERT_GE(H5Pset_filter(dcpl.get(), kCustomTestFilterId,
                            H5Z_FLAG_MANDATORY, 1, &cd_shrink),
              0);
    ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_EARLY), 0);
    ASSERT_GE(H5Pset_fill_value(dcpl.get(), vl_str.get(), &vl_fill), 0);

    ScopedDataset ds_vl_shrink(H5Dcreate2(file.get(), "vl_shrink_early",
                                          vl_str.get(), sp.get(), H5P_DEFAULT,
                                          dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(ds_vl_shrink.valid());

    std::array<char*, 16> out{};
    ASSERT_GE(H5Dread(ds_vl_shrink.get(), vl_str.get(), H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, out.data()),
              0);
    auto reclaim = absl::MakeCleanup([&]() {
      H5Treclaim(vl_str.get(), sp.get(), H5P_DEFAULT, out.data());
    });
    for (size_t i = 0; i < out.size(); ++i) {
      ASSERT_NE(out[i], nullptr);
      EXPECT_STREQ(out[i], "vl_shrink_fill");
    }
  }

  // 3. Filter failure error-handling paths (forward mode = 2, reverse mode = 3)
  //    under ScopedErrorSilence:
  {
    ScopedErrorSilence silence;
    const unsigned int cd_rev_fail = 3;

    // 3a. Reverse filter failure on read (4855, 5002-5003, 3176).
    {
      std::array<hsize_t, 1> dims = {8};
      std::array<hsize_t, 1> cdims = {4};
      ScopedSpace sp(H5Screate_simple(1, dims.data(), nullptr));
      ScopedPlist dcpl = MakeChunkedDcpl(cdims);
      ASSERT_GE(H5Pset_filter(dcpl.get(), kCustomTestFilterId,
                              H5Z_FLAG_MANDATORY, 1, &cd_rev_fail),
                0);
      ScopedDataset d_rev(H5Dcreate2(file.get(), "rev_fail", H5T_NATIVE_INT32,
                                     sp.get(), H5P_DEFAULT, dcpl.get(),
                                     H5P_DEFAULT));
      ASSERT_TRUE(d_rev.valid());
      std::array<int32_t, 8> w = {1, 2, 3, 4, 5, 6, 7, 8};
      ASSERT_GE(H5Dwrite(d_rev.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                         dxpl_off.get(), w.data()),
                0);
      ASSERT_GE(H5Dflush(d_rev.get()), 0);
      d_rev.reset();

      d_rev.reset(H5Dopen2(file.get(), "rev_fail", H5P_DEFAULT));
      ASSERT_TRUE(d_rev.valid());
      std::array<int32_t, 8> r{};
      EXPECT_LT(H5Dread(d_rev.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                        dxpl_off.get(), r.data()),
                0);
    }

    // 3b. Forward filter failure on cache prune eviction (4294, 4391-4394,
    //     4562, 4574, 4932, 5002-5003, 3564) and on query flush loops
    //     (8005-8006, 8090-8091, 8194-8195, 8329, 8452-8453).
    {
      std::array<hsize_t, 1> dims = {12};
      std::array<hsize_t, 1> cdims = {4};
      ScopedSpace sp(H5Screate_simple(1, dims.data(), nullptr));
      ScopedPlist dcpl = MakeChunkedDcpl(cdims);
      const unsigned int cd_fwd_fail = 2;
      ASSERT_GE(H5Pset_filter(dcpl.get(), kCustomTestFilterId,
                              H5Z_FLAG_MANDATORY, 1, &cd_fwd_fail),
                0);

      ScopedPlist dapl_2(H5Pcreate(H5P_DATASET_ACCESS));
      ASSERT_GE(H5Pset_chunk_cache(dapl_2.get(), 13, 32, 0.5), 0);
      ScopedDataset d_prune_fail(
          H5Dcreate2(file.get(), "fwd_prune_fail", H5T_NATIVE_INT32, sp.get(),
                     H5P_DEFAULT, dcpl.get(), dapl_2.get()));
      ASSERT_TRUE(d_prune_fail.valid());

      ScopedSpace m4(H5Screate_simple(1, cdims.data(), nullptr));
      std::array<int32_t, 4> c = {1, 2, 3, 4};
      std::array<hsize_t, 1> hs = {0};
      ASSERT_GE(H5Sselect_hyperslab(sp.get(), H5S_SELECT_SET, hs.data(),
                                    nullptr, cdims.data(), nullptr),
                0);
      ASSERT_GE(H5Dwrite(d_prune_fail.get(), H5T_NATIVE_INT32, m4.get(),
                         sp.get(), dxpl_off.get(), c.data()),
                0);
      hs = {4};
      ASSERT_GE(H5Sselect_hyperslab(sp.get(), H5S_SELECT_SET, hs.data(),
                                    nullptr, cdims.data(), nullptr),
                0);
      ASSERT_GE(H5Dwrite(d_prune_fail.get(), H5T_NATIVE_INT32, m4.get(),
                         sp.get(), dxpl_off.get(), c.data()),
                0);

      // Query APIs hit flush failure in each dirty-flush loop while {0} & {4}
      // remain dirty in cache!
      {
        ScopedApiContext api_ctx;
        ASSERT_TRUE(api_ctx.valid());
        H5D_t* d = UnwrapDataset(d_prune_fail.get());
        ASSERT_NE(d, nullptr);
        bool empty = false;
        EXPECT_LT(H5D__chunk_index_empty(d, &empty), 0);
        hsize_t alloc_bytes = 0;
        EXPECT_LT(H5D__chunk_allocated(d, &alloc_bytes), 0);
        haddr_t addr_i = HADDR_UNDEF;
        hsize_t sz_i = 0;
        EXPECT_LT(H5D__get_chunk_info(d, d->shared->space, 0, nullptr, nullptr,
                                      &addr_i, &sz_i),
                  0);
      }
      hsize_t nchk = 0;
      EXPECT_LT(H5Dget_num_chunks(d_prune_fail.get(), sp.get(), &nchk), 0);
      hsize_t off_out = 0;
      unsigned fm = 0;
      haddr_t addr_out = HADDR_UNDEF;
      hsize_t sz_out = 0;
      EXPECT_LT(H5Dget_chunk_info(d_prune_fail.get(), sp.get(), 0, &off_out,
                                  &fm, &addr_out, &sz_out),
                0);
      std::array<hsize_t, 1> qcoord = {0};
      EXPECT_LT(H5Dget_chunk_info_by_coord(d_prune_fail.get(), qcoord.data(),
                                           &fm, &addr_out, &sz_out),
                0);
      EXPECT_LT(H5Dchunk_iter(
                    d_prune_fail.get(), H5P_DEFAULT,
                    [](const hsize_t*, unsigned, haddr_t, hsize_t, void*) {
                      return H5_ITER_CONT;
                    },
                    nullptr),
                0);

      // Writing 3rd chunk forces prune -> flush failure (4932, 3564).
      hs = {8};
      ASSERT_GE(H5Sselect_hyperslab(sp.get(), H5S_SELECT_SET, hs.data(),
                                    nullptr, cdims.data(), nullptr),
                0);
      EXPECT_LT(H5Dwrite(d_prune_fail.get(), H5T_NATIVE_INT32, m4.get(),
                         sp.get(), dxpl_off.get(), c.data()),
                0);
      d_prune_fail.reset();

      // Slot-collision eviction failure (nslots = 1, 4929).
      ScopedPlist dapl_1(H5Pcreate(H5P_DATASET_ACCESS));
      ASSERT_GE(H5Pset_chunk_cache(dapl_1.get(), 1, 1024, 0.5), 0);
      ScopedDataset d_slot_fail(
          H5Dcreate2(file.get(), "fwd_slot_fail", H5T_NATIVE_INT32, sp.get(),
                     H5P_DEFAULT, dcpl.get(), dapl_1.get()));
      ASSERT_TRUE(d_slot_fail.valid());
      hs = {0};
      ASSERT_GE(H5Sselect_hyperslab(sp.get(), H5S_SELECT_SET, hs.data(),
                                    nullptr, cdims.data(), nullptr),
                0);
      ASSERT_GE(H5Dwrite(d_slot_fail.get(), H5T_NATIVE_INT32, m4.get(),
                         sp.get(), dxpl_off.get(), c.data()),
                0);
      hs = {4};
      ASSERT_GE(H5Sselect_hyperslab(sp.get(), H5S_SELECT_SET, hs.data(),
                                    nullptr, cdims.data(), nullptr),
                0);
      EXPECT_LT(H5Dwrite(d_slot_fail.get(), H5T_NATIVE_INT32, m4.get(),
                         sp.get(), dxpl_off.get(), c.data()),
                0);
      d_slot_fail.reset();

      // ALLOC_TIME_EARLY create failures (5369 scalar, 5512 VLEN).
      ScopedPlist dcpl_early(H5Pcopy(dcpl.get()));
      ASSERT_GE(H5Pset_alloc_time(dcpl_early.get(), H5D_ALLOC_TIME_EARLY), 0);
      EXPECT_LT(H5Dcreate2(file.get(), "early_fwd_fail", H5T_NATIVE_INT32,
                           sp.get(), H5P_DEFAULT, dcpl_early.get(),
                           H5P_DEFAULT),
                0);

      ScopedType vl_str(H5Tcopy(H5T_C_S1));
      ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);
      const char* vl_f = "fail";
      ASSERT_GE(H5Pset_fill_value(dcpl_early.get(), vl_str.get(), &vl_f), 0);
      EXPECT_LT(H5Dcreate2(file.get(), "early_vl_fwd_fail", vl_str.get(),
                           sp.get(), H5P_DEFAULT, dcpl_early.get(),
                           H5P_DEFAULT),
                0);

      // H5Dformat_convert failure when edge chunk is flushed unfiltered then
      // filtered with mode = 2 during format convert (7873).
      ScopedPlist fapl_v110 =
          MakeFaplWithLibver(H5F_LIBVER_V110, H5F_LIBVER_V114);
      ScopedFile f_fmt(
          H5Fcreate("fmt_fail.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v110.get()));
      ASSERT_TRUE(f_fmt.valid());
      std::array<hsize_t, 2> d2 = {6, 5};
      std::array<hsize_t, 2> cd2 = {4, 4};
      ScopedSpace sp2(H5Screate_simple(2, d2.data(), nullptr));
      ScopedPlist dcpl2 = MakeChunkedDcpl(cd2);
      ASSERT_GE(H5Pset_filter(dcpl2.get(), kCustomTestFilterId,
                              H5Z_FLAG_MANDATORY, 1, &cd_fwd_fail),
                0);
      ASSERT_GE(
          H5Pset_chunk_opts(dcpl2.get(), H5D_CHUNK_DONT_FILTER_PARTIAL_CHUNKS),
          0);
      ScopedDataset d_fmt(H5Dcreate2(f_fmt.get(), "fmt_fail_ds",
                                     H5T_NATIVE_INT32, sp2.get(), H5P_DEFAULT,
                                     dcpl2.get(), H5P_DEFAULT));
      ASSERT_TRUE(d_fmt.valid());
      std::array<hsize_t, 2> hs2 = {4, 4};
      std::array<hsize_t, 2> cnt2 = {2, 1};
      ASSERT_GE(H5Sselect_hyperslab(sp2.get(), H5S_SELECT_SET, hs2.data(),
                                    nullptr, cnt2.data(), nullptr),
                0);
      ScopedSpace m21(H5Screate_simple(2, cnt2.data(), nullptr));
      std::array<int32_t, 2> edge2 = {7, 8};
      ASSERT_GE(H5Dwrite(d_fmt.get(), H5T_NATIVE_INT32, m21.get(), sp2.get(),
                         dxpl_off.get(), edge2.data()),
                0);
      ASSERT_GE(H5Dflush(d_fmt.get()), 0);
      EXPECT_LT(H5Dformat_convert(d_fmt.get()), 0);

      // Direct storage-size & direct-read eviction failures (691, 590) while
      // is_space_alloc is true (chunk {4} written via H5Dwrite_chunk) and
      // chunk {0} is dirty in RDCC.
      {
        std::array<hsize_t, 1> d8 = {8};
        ScopedSpace sp8(H5Screate_simple(1, d8.data(), nullptr));
        ScopedDataset d_stor_fail(
            H5Dcreate2(file.get(), "fwd_stor_fail", H5T_NATIVE_INT32, sp8.get(),
                       H5P_DEFAULT, dcpl.get(), dapl_2.get()));
        ASSERT_TRUE(d_stor_fail.valid());
        std::array<hsize_t, 1> q4 = {4};
        ASSERT_GE(H5Dwrite_chunk(d_stor_fail.get(), H5P_DEFAULT, 0, q4.data(),
                                 sizeof(c), c.data()),
                  0);
        hs = {0};
        ASSERT_GE(H5Sselect_hyperslab(sp8.get(), H5S_SELECT_SET, hs.data(),
                                      nullptr, cdims.data(), nullptr),
                  0);
        ASSERT_GE(H5Dwrite(d_stor_fail.get(), H5T_NATIVE_INT32, m4.get(),
                           sp8.get(), dxpl_off.get(), c.data()),
                  0);
        EXPECT_LT(H5Dget_chunk_storage_size(d_stor_fail.get(), qcoord.data(),
                                            &sz_out),
                  0);

        ASSERT_GE(H5Dwrite(d_stor_fail.get(), H5T_NATIVE_INT32, m4.get(),
                           sp8.get(), dxpl_off.get(), c.data()),
                  0);
        size_t bsz = sizeof(c);
        std::array<int32_t, 4> cbuf{};
        uint32_t fmask_out = 0;
        EXPECT_LT(H5Dread_chunk2(d_stor_fail.get(), H5P_DEFAULT, qcoord.data(),
                                 &fmask_out, cbuf.data(), &bsz),
                  0);
      }

      // 3c. Uncached (rdcc_nbytes = 0) & extent-change filter failures
      //     (5085, 3619, 5807, 6127, 6507, 6839).
      {
        ScopedPlist dapl_nocache(H5Pcreate(H5P_DATASET_ACCESS));
        ASSERT_GE(H5Pset_chunk_cache(dapl_nocache.get(), 0, 0, 0.0), 0);

        // Uncached write hits mode = 2 in H5D__chunk_unlock -> H5D__chunk_write.
        std::array<hsize_t, 1> d4 = {4};
        ScopedSpace sp4(H5Screate_simple(1, d4.data(), nullptr));
        ScopedDataset d_uc_fwd(
            H5Dcreate2(file.get(), "uc_fwd_fail", H5T_NATIVE_INT32, sp4.get(),
                       H5P_DEFAULT, dcpl.get(), dapl_nocache.get()));
        ASSERT_TRUE(d_uc_fwd.valid());
        EXPECT_LT(H5Dwrite(d_uc_fwd.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                           dxpl_off.get(), c.data()),
                  0);

        // Partial edge {5, 5} written unfiltered, then extend -> {8, 8} fails
        // in H5D__chunk_update_old_edge_chunks -> H5D__chunk_unlock (5807).
        std::array<hsize_t, 2> d5 = {5, 5};
        std::array<hsize_t, 2> unlim2 = {H5S_UNLIMITED, H5S_UNLIMITED};
        ScopedSpace sp5u(H5Screate_simple(2, d5.data(), unlim2.data()));
        ScopedDataset d_upd_fail(
            H5Dcreate2(file.get(), "uc_edge_upd_fail", H5T_NATIVE_INT32,
                       sp5u.get(), H5P_DEFAULT, dcpl2.get(),
                       dapl_nocache.get()));
        ASSERT_TRUE(d_upd_fail.valid());
        std::array<hsize_t, 2> cnt11 = {1, 1};
        ASSERT_GE(H5Sselect_hyperslab(sp5u.get(), H5S_SELECT_SET, hs2.data(),
                                      nullptr, cnt11.data(), nullptr),
                  0);
        ScopedSpace m11(H5Screate_simple(2, cnt11.data(), nullptr));
        int32_t elem11 = 99;
        ASSERT_GE(H5Dwrite(d_upd_fail.get(), H5T_NATIVE_INT32, m11.get(),
                           sp5u.get(), dxpl_off.get(), &elem11),
                  0);
        std::array<hsize_t, 2> d88 = {8, 8};
        EXPECT_LT(H5Dset_extent(d_upd_fail.get(), d88.data()), 0);

        // Uncached mode = 3 write succeeds, then shrink {8, 8} -> {6, 6} fails
        // in H5D__chunk_prune_fill -> H5D__chunk_prune_by_extent (6127, 6507).
        ScopedSpace sp8u(H5Screate_simple(2, d88.data(), unlim2.data()));
        ScopedPlist dcpl_rev2 = MakeChunkedDcpl(cd2);
        ASSERT_GE(H5Pset_filter(dcpl_rev2.get(), kCustomTestFilterId,
                                H5Z_FLAG_MANDATORY, 1, &cd_rev_fail),
                  0);
        ScopedDataset d_prune_rev(
            H5Dcreate2(file.get(), "uc_prune_rev_fail", H5T_NATIVE_INT32,
                       sp8u.get(), H5P_DEFAULT, dcpl_rev2.get(),
                       dapl_nocache.get()));
        ASSERT_TRUE(d_prune_rev.valid());
        std::vector<int32_t> w64(64, 11);
        ASSERT_GE(H5Dwrite(d_prune_rev.get(), H5T_NATIVE_INT32, H5S_ALL,
                           H5S_ALL, dxpl_off.get(), w64.data()),
                  0);
        std::array<hsize_t, 2> d66 = {6, 6};
        EXPECT_LT(H5Dset_extent(d_prune_rev.get(), d66.data()), 0);

        // Extend causes hash-slot collision in H5D__chunk_update_cache (6839)
        // when nslots = 3: chunks {0, 1} (slot 1 -> 1) & {1, 0} (slot 2 -> 1).
        std::array<hsize_t, 2> d22 = {2, 2};
        std::array<hsize_t, 2> c11 = {1, 1};
        ScopedSpace sp22(H5Screate_simple(2, d22.data(), unlim2.data()));
        ScopedPlist dcpl_1x1 = MakeChunkedDcpl(c11);
        ASSERT_GE(H5Pset_filter(dcpl_1x1.get(), kCustomTestFilterId,
                                H5Z_FLAG_MANDATORY, 1, &cd_fwd_fail),
                  0);
        ScopedPlist dapl_3(H5Pcreate(H5P_DATASET_ACCESS));
        ASSERT_GE(H5Pset_chunk_cache(dapl_3.get(), 3, 1024, 0.5), 0);
        ScopedDataset d_upd_cache(
            H5Dcreate2(file.get(), "upd_cache_fail", H5T_NATIVE_INT32,
                       sp22.get(), H5P_DEFAULT, dcpl_1x1.get(), dapl_3.get()));
        ASSERT_TRUE(d_upd_cache.valid());
        int32_t one_val = 5;
        std::array<hsize_t, 2> hs01 = {0, 1};
        ASSERT_GE(H5Sselect_hyperslab(sp22.get(), H5S_SELECT_SET, hs01.data(),
                                      nullptr, c11.data(), nullptr),
                  0);
        ASSERT_GE(H5Dwrite(d_upd_cache.get(), H5T_NATIVE_INT32, m11.get(),
                           sp22.get(), dxpl_off.get(), &one_val),
                  0);
        std::array<hsize_t, 2> hs10 = {1, 0};
        ASSERT_GE(H5Sselect_hyperslab(sp22.get(), H5S_SELECT_SET, hs10.data(),
                                      nullptr, c11.data(), nullptr),
                  0);
        ASSERT_GE(H5Dwrite(d_upd_cache.get(), H5T_NATIVE_INT32, m11.get(),
                           sp22.get(), dxpl_off.get(), &one_val),
                  0);
        std::array<hsize_t, 2> d24 = {2, 4};
        EXPECT_LT(H5Dset_extent(d_upd_cache.get(), d24.data()), 0);
      }

      // 3d. H5Ocopy filter failures: dirty cached chunk forward failure
      //     (7049, 7312) and closed reference chunk reverse failure (6991).
      {
        ScopedFile src_copy = CreateInMemoryFile("src_copy_fail.h5");
        ScopedFile dst_copy = CreateInMemoryFile("dst_copy_fail.h5");
        ASSERT_TRUE(src_copy.valid());
        ASSERT_TRUE(dst_copy.valid());

        {
          std::array<hsize_t, 1> d4 = {4};
          ScopedSpace sp4(H5Screate_simple(1, d4.data(), nullptr));
          ScopedDataset open_fwd(
              H5Dcreate2(src_copy.get(), "open_fwd_fail", H5T_NATIVE_INT32,
                         sp4.get(), H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
          ASSERT_TRUE(open_fwd.valid());
          ASSERT_GE(H5Dwrite(open_fwd.get(), H5T_NATIVE_INT32, H5S_ALL,
                             H5S_ALL, dxpl_off.get(), c.data()),
                    0);
          EXPECT_LT(H5Ocopy(src_copy.get(), "open_fwd_fail", dst_copy.get(),
                            "open_fwd_dst", H5P_DEFAULT, H5P_DEFAULT),
                    0);
        }

        std::array<hsize_t, 1> d1 = {1};
        ScopedSpace sp1(H5Screate_simple(1, d1.data(), nullptr));
        ScopedDataset target(
            H5Dcreate2(src_copy.get(), "ref_target", H5T_NATIVE_INT32,
                       sp1.get(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
        ASSERT_TRUE(target.valid());
        target.reset();
        hobj_ref_t ref{};
        ASSERT_GE(
            H5Rcreate(&ref, src_copy.get(), "/ref_target", H5R_OBJECT, -1), 0);

        ScopedPlist dcpl_ref = MakeChunkedDcpl(d1);
        ASSERT_GE(H5Pset_filter(dcpl_ref.get(), kCustomTestFilterId,
                                H5Z_FLAG_MANDATORY, 1, &cd_rev_fail),
                  0);
        ScopedDataset ref_rev(
            H5Dcreate2(src_copy.get(), "ref_rev_fail", H5T_STD_REF_OBJ,
                       sp1.get(), H5P_DEFAULT, dcpl_ref.get(), H5P_DEFAULT));
        ASSERT_TRUE(ref_rev.valid());
        ASSERT_GE(H5Dwrite(ref_rev.get(), H5T_STD_REF_OBJ, H5S_ALL, H5S_ALL,
                           dxpl_off.get(), &ref),
                  0);
        ref_rev.reset();

        EXPECT_LT(H5Ocopy(src_copy.get(), "ref_rev_fail", dst_copy.get(),
                          "ref_rev_dst", H5P_DEFAULT, H5P_DEFAULT),
                  0);
      }
    }
  }

  // 4. H5Ldelete on a closed filtered chunked dataset hits H5D__chunk_delete
  //    pline read & reset (6715-6718, 6734-6735).
  {
    std::array<hsize_t, 1> dims = {8};
    std::array<hsize_t, 1> cdims = {4};
    ScopedSpace sp(H5Screate_simple(1, dims.data(), nullptr));
    ScopedPlist dcpl = MakeChunkedDcpl(cdims);
    ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);
    ScopedDataset ds_del(H5Dcreate2(file.get(), "del_filtered",
                                    H5T_NATIVE_INT32, sp.get(), H5P_DEFAULT,
                                    dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(ds_del.valid());
    std::array<int32_t, 8> v = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_GE(H5Dwrite(ds_del.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, v.data()),
              0);
    ds_del.reset();
    ASSERT_GE(H5Ldelete(file.get(), "del_filtered", H5P_DEFAULT), 0);
  }
}

}  // namespace
}  // namespace hdf5
