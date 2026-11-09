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
// Tests external file list object header message encoding, decoding, copying,
// sizing, resetting, total size queries, file copy, validation errors, and
// debug output (third_party/hdf5/src/H5Oefl.c).
// Note: H5O_DEBUG sanity checks inside H5O__efl_decode() are compiled out in
// -DNDEBUG builds.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Oefl.c

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields in metadata
// cache and object header structs pulled in via internal headers.
#ifndef NDEBUG
#define H5OEFL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5HLprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5OEFL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OEFL_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__SUPERBLOCK_TAG) {
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

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

void AppendUint16LE(std::vector<uint8_t>& buf, uint16_t val) {
  buf.push_back(static_cast<uint8_t>(val & 0xffU));
  buf.push_back(static_cast<uint8_t>((val >> 8) & 0xffU));
}

void AppendUintLE(std::vector<uint8_t>& buf, uint64_t val, size_t nbytes) {
  for (size_t i = 0; i < nbytes; ++i) {
    buf.push_back(static_cast<uint8_t>((val >> (8 * i)) & 0xffU));
  }
}

std::string ReadTmpFile(FILE* fp) {
  std::fflush(fp);
  std::rewind(fp);
  std::string out;
  char chunk[256];
  while (true) {
    const size_t n = std::fread(chunk, 1, sizeof(chunk), fp);
    if (n == 0) {
      break;
    }
    out.append(chunk, n);
  }
  return out;
}

TEST(H5OeflTest, TotalSizeCalculationsAndOverflow) {
  ScopedErrorSilence silence;

  H5O_efl_t empty{};
  hsize_t total = 99;
  ASSERT_GE(H5O_efl_total_size(&empty, &total), 0);
  EXPECT_EQ(total, 0);

  std::vector<H5O_efl_entry_t> slots(3);
  slots[0].size = 128;
  slots[1].size = 256;
  slots[2].size = 512;
  H5O_efl_t finite{};
  finite.nalloc = 3;
  finite.nused = 3;
  finite.slot = slots.data();
  ASSERT_GE(H5O_efl_total_size(&finite, &total), 0);
  EXPECT_EQ(total, 896);

  slots[2].size = H5O_EFL_UNLIMITED;
  ASSERT_GE(H5O_efl_total_size(&finite, &total), 0);
  EXPECT_EQ(total, H5O_EFL_UNLIMITED);

  std::vector<H5O_efl_entry_t> overflow_slots(2);
  overflow_slots[0].size = std::numeric_limits<hsize_t>::max() - 5;
  overflow_slots[1].size = 10;
  H5O_efl_t overflow{};
  overflow.nalloc = 2;
  overflow.nused = 2;
  overflow.slot = overflow_slots.data();
  EXPECT_LT(H5O_efl_total_size(&overflow, &total), 0);
}

TEST(H5OeflTest, EncodeDecodeCopySizeResetAndSyntheticAlloc) {
  ScopedFile file = CreateInMemoryFile("efl_roundtrip.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  haddr_t heap_addr = HADDR_UNDEF;
  size_t off0 = 1;
  size_t off1 = 0;
  size_t off2 = 0;
  {
    ASSERT_GE(H5HL_create(f, 128, &heap_addr), 0);
    H5HL_t* heap = H5HL_protect(f, heap_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    ASSERT_GE(H5HL_insert(f, heap, 1, "", &off0), 0);
    EXPECT_EQ(off0, 0);
    ASSERT_GE(H5HL_insert(f, heap, 10, "part0.bin", &off1), 0);
    ASSERT_GE(H5HL_insert(f, heap, 10, "part1.bin", &off2), 0);
    ASSERT_GE(H5HL_unprotect(heap), 0);
  }

  H5O_efl_t src{};
  src.heap_addr = heap_addr;
  src.nalloc = 4;
  src.nused = 2;
  src.slot =
      static_cast<H5O_efl_entry_t*>(H5MM_calloc(src.nalloc * sizeof(H5O_efl_entry_t)));
  ASSERT_NE(src.slot, nullptr);
  absl::Cleanup reset_src = [&src] { H5O_msg_reset(H5O_EFL_ID, &src); };

  src.slot[0].name_offset = off1;
  src.slot[0].name = H5MM_xstrdup("part0.bin");
  src.slot[0].offset = 16;
  src.slot[0].size = 64;
  src.slot[1].name_offset = off2;
  src.slot[1].name = H5MM_xstrdup("part1.bin");
  src.slot[1].offset = 128;
  src.slot[1].size = 256;

  // Query encoded size and encode/decode through H5O message layer.
  const size_t raw_size = H5O_msg_raw_size(f, H5O_EFL_ID, false, &src);
  const size_t addr_size = H5F_sizeof_addr(f);
  const size_t len_size = H5F_sizeof_size(f);
  EXPECT_EQ(raw_size, 8 + addr_size + 2 * (3 * len_size));

  std::vector<uint8_t> encoded(raw_size, 0);
  ASSERT_GE(H5O_msg_encode(f, H5O_EFL_ID, false, encoded.data(), &src), 0);

  // H5O__efl_encode stores nused in both nalloc and nused fields.
  H5O_efl_t* decoded = static_cast<H5O_efl_t*>(
      H5O_msg_decode(f, nullptr, H5O_EFL_ID, encoded.size(), encoded.data()));
  ASSERT_NE(decoded, nullptr);
  absl::Cleanup free_decoded = [decoded] { H5O_msg_free(H5O_EFL_ID, decoded); };

  EXPECT_EQ(decoded->heap_addr, heap_addr);
  EXPECT_EQ(decoded->nalloc, 2);
  EXPECT_EQ(decoded->nused, 2);
  ASSERT_NE(decoded->slot, nullptr);
  EXPECT_STREQ(decoded->slot[0].name, "part0.bin");
  EXPECT_EQ(decoded->slot[0].name_offset, off1);
  EXPECT_EQ(decoded->slot[0].offset, 16);
  EXPECT_EQ(decoded->slot[0].size, 64);
  EXPECT_STREQ(decoded->slot[1].name, "part1.bin");
  EXPECT_EQ(decoded->slot[1].name_offset, off2);
  EXPECT_EQ(decoded->slot[1].offset, 128);
  EXPECT_EQ(decoded->slot[1].size, 256);

  // Synthetic decode where nalloc > nused.
  std::vector<uint8_t> synth = encoded;
  synth[4] = 5;
  synth[5] = 0;
  H5O_efl_t* decoded_nalloc = static_cast<H5O_efl_t*>(
      H5O_msg_decode(f, nullptr, H5O_EFL_ID, synth.size(), synth.data()));
  ASSERT_NE(decoded_nalloc, nullptr);
  EXPECT_EQ(decoded_nalloc->nalloc, 5);
  EXPECT_EQ(decoded_nalloc->nused, 2);
  H5O_msg_free(H5O_EFL_ID, decoded_nalloc);

  // Copy with dest == nullptr and caller-provided dest.
  H5O_efl_t* copy_alloc =
      static_cast<H5O_efl_t*>(H5O_msg_copy(H5O_EFL_ID, &src, nullptr));
  ASSERT_NE(copy_alloc, nullptr);
  EXPECT_EQ(copy_alloc->nalloc, 4);
  EXPECT_EQ(copy_alloc->nused, 2);
  EXPECT_NE(copy_alloc->slot[0].name, src.slot[0].name);
  EXPECT_STREQ(copy_alloc->slot[0].name, "part0.bin");
  EXPECT_STREQ(copy_alloc->slot[1].name, "part1.bin");
  H5O_msg_free(H5O_EFL_ID, copy_alloc);

  H5O_efl_t copy_stack{};
  void* copy_ptr = H5O_msg_copy(H5O_EFL_ID, &src, &copy_stack);
  EXPECT_EQ(copy_ptr, &copy_stack);
  EXPECT_STREQ(copy_stack.slot[0].name, "part0.bin");
  ASSERT_GE(H5O_msg_reset(H5O_EFL_ID, &copy_stack), 0);
  EXPECT_EQ(copy_stack.slot, nullptr);
  EXPECT_EQ(copy_stack.nused, 0);
  EXPECT_EQ(copy_stack.nalloc, 0);
  EXPECT_EQ(copy_stack.heap_addr, HADDR_UNDEF);

  // Resetting again when slot == nullptr should still succeed, and copying an
  // empty message with nalloc == 0 covers the non-allocating copy branch.
  ASSERT_GE(H5O_msg_reset(H5O_EFL_ID, &copy_stack), 0);
  H5O_efl_t* copy_empty =
      static_cast<H5O_efl_t*>(H5O_msg_copy(H5O_EFL_ID, &copy_stack, nullptr));
  ASSERT_NE(copy_empty, nullptr);
  EXPECT_EQ(copy_empty->nalloc, 0);
  EXPECT_EQ(copy_empty->slot, nullptr);
  H5O_msg_free(H5O_EFL_ID, copy_empty);

  // Copy failure when a used slot has a NULL name exercises cleanup of prior
  // allocated slot names for both dest == nullptr and caller-supplied dest.
  {
    ScopedErrorSilence silence;
    char* saved_name = src.slot[1].name;
    src.slot[1].name = nullptr;
    absl::Cleanup restore_name = [&src, saved_name] {
      src.slot[1].name = saved_name;
    };
    EXPECT_EQ(H5O_msg_copy(H5O_EFL_ID, &src, nullptr), nullptr);
    H5O_efl_t failed_stack{};
    EXPECT_EQ(H5O_msg_copy(H5O_EFL_ID, &src, &failed_stack), nullptr);
    EXPECT_EQ(failed_stack.slot, nullptr);
  }
}

TEST(H5OeflTest, DecodeValidationAndOverflowErrors) {
  ScopedErrorSilence silence;
  ScopedFile file = CreateInMemoryFile("efl_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  const size_t addr_size = H5F_sizeof_addr(f);
  const size_t len_size = H5F_sizeof_size(f);

  haddr_t heap_addr = HADDR_UNDEF;
  size_t off_empty = 1;
  size_t off_name = 0;
  size_t heap_block_size = 0;
  {
    ScopedApiContext ctx;
    ASSERT_GE(H5HL_create(f, 64, &heap_addr), 0);
    H5HL_t* heap = H5HL_protect(f, heap_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    ASSERT_GE(H5HL_insert(f, heap, 1, "", &off_empty), 0);
    ASSERT_GE(H5HL_insert(f, heap, 8, "ok.data", &off_name), 0);
    heap_block_size = H5HL_heap_get_size(heap);
    ASSERT_GE(H5HL_unprotect(heap), 0);
  }
  ASSERT_EQ(off_empty, 0);

  auto make_valid = [&](uint16_t nalloc, uint16_t nused, haddr_t haddr,
                        size_t name_off) {
    std::vector<uint8_t> buf;
    buf.push_back(1);  // version
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0);  // reserved
    AppendUint16LE(buf, nalloc);
    AppendUint16LE(buf, nused);
    AppendUintLE(buf, haddr, addr_size);
    for (uint16_t i = 0; i < nused; ++i) {
      AppendUintLE(buf, name_off, len_size);
      AppendUintLE(buf, 0, len_size);
      AppendUintLE(buf, 32, len_size);
    }
    return buf;
  };

  // Truncated at each header field (using offset +1 in a padded vector so
  // p_size == 0 also keeps p_end inside allocated storage).
  {
    std::vector<uint8_t> full = make_valid(2, 2, heap_addr, off_name);
    std::vector<uint8_t> padded(full.size() + 1, 0);
    std::memcpy(padded.data() + 1, full.data(), full.size());
    const uint8_t* start = padded.data() + 1;

    const std::vector<size_t> bad_sizes = {
        0,                                      // version overflow
        2,                                      // reserved overflow
        5,                                      // nalloc overflow
        7,                                      // nused overflow
        8 + addr_size - 1,                      // heap_addr overflow
        8 + addr_size + len_size - 1,           // slot[0].name_offset overflow
        8 + addr_size + 2 * len_size - 1,       // slot[0].offset overflow
        8 + addr_size + 3 * len_size - 1,       // slot[0].size overflow
        8 + addr_size + 3 * len_size + 1,       // slot[1] partial after slot[0]
    };
    for (size_t bad_sz : bad_sizes) {
      ScopedApiContext ctx;
      EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_EFL_ID, bad_sz, start), nullptr)
          << "bad_sz=" << bad_sz;
    }
  }

  // Bad version (!= 1).
  {
    std::vector<uint8_t> buf = make_valid(1, 1, heap_addr, off_name);
    buf[0] = 2;
    EXPECT_EQ(
        H5O_msg_decode(f, nullptr, H5O_EFL_ID, buf.size(), buf.data()),
        nullptr);
    buf[0] = 0;
    EXPECT_EQ(
        H5O_msg_decode(f, nullptr, H5O_EFL_ID, buf.size(), buf.data()),
        nullptr);
  }

  // nalloc == 0.
  {
    std::vector<uint8_t> buf = make_valid(0, 0, heap_addr, off_name);
    EXPECT_EQ(
        H5O_msg_decode(f, nullptr, H5O_EFL_ID, buf.size(), buf.data()),
        nullptr);
  }

  // nused > nalloc.
  {
    std::vector<uint8_t> buf = make_valid(1, 2, heap_addr, off_name);
    EXPECT_EQ(
        H5O_msg_decode(f, nullptr, H5O_EFL_ID, buf.size(), buf.data()),
        nullptr);
  }

  // Undefined heap address.
  {
    std::vector<uint8_t> buf = make_valid(1, 1, HADDR_UNDEF, off_name);
    EXPECT_EQ(
        H5O_msg_decode(f, nullptr, H5O_EFL_ID, buf.size(), buf.data()),
        nullptr);
  }

  // Bogus (non-heap) address fails H5HL_protect().
  {
    ScopedApiContext ctx;
    std::vector<uint8_t> buf = make_valid(1, 1, 0x20, off_name);
    EXPECT_EQ(
        H5O_msg_decode(f, nullptr, H5O_EFL_ID, buf.size(), buf.data()),
        nullptr);
  }

  // Out-of-bounds name offset into real heap fails H5HL_offset_into().
  {
    ScopedApiContext ctx;
    std::vector<uint8_t> buf =
        make_valid(1, 1, heap_addr, heap_block_size + 64);
    EXPECT_EQ(
        H5O_msg_decode(f, nullptr, H5O_EFL_ID, buf.size(), buf.data()),
        nullptr);
  }

  // Offset 0 holds the empty sentinel string (*s == '\0'), rejected as invalid.
  {
    ScopedApiContext ctx;
    std::vector<uint8_t> buf = make_valid(1, 1, heap_addr, off_empty);
    EXPECT_EQ(
        H5O_msg_decode(f, nullptr, H5O_EFL_ID, buf.size(), buf.data()),
        nullptr);
  }
}

TEST(H5OeflTest, PublicDatasetCopyFileAndDebugOutput) {
  TempDir tmp("h5oefl_copy_debug");
  const std::string src_h5 = tmp.child("src.h5").string();
  const std::string dst_h5 = tmp.child("dst.h5").string();
  const std::string ext0 = tmp.child("ext0.dat").string();
  const std::string ext1 = tmp.child("ext1.dat").string();

  // Create backing external files with distinct payloads.
  {
    std::ofstream out0(ext0, std::ios::binary);
    std::vector<int32_t> vals0 = {10, 20, 30, 40};
    out0.write(reinterpret_cast<const char*>(vals0.data()),
               static_cast<std::streamsize>(vals0.size() * sizeof(int32_t)));
    ASSERT_TRUE(out0.good());

    std::ofstream out1(ext1, std::ios::binary);
    std::vector<int32_t> vals1 = {999, 50, 60, 70, 80};
    out1.write(reinterpret_cast<const char*>(vals1.data()),
               static_cast<std::streamsize>(vals1.size() * sizeof(int32_t)));
    ASSERT_TRUE(out1.good());
  }

  haddr_t src_dset_addr = HADDR_UNDEF;
  {
    ScopedFile src(H5Fcreate(src_h5.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                             H5P_DEFAULT));
    ASSERT_TRUE(src.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_external(dcpl.get(), ext0.c_str(), 0, 4 * sizeof(int32_t)),
              0);
    ASSERT_GE(H5Pset_external(dcpl.get(), ext1.c_str(), sizeof(int32_t),
                              4 * sizeof(int32_t)),
              0);

    const hsize_t dims[1] = {8};
    ScopedSpace space(H5Screate_simple(1, dims, nullptr));
    ASSERT_TRUE(space.valid());

    ScopedDataset dset(H5Dcreate2(src.get(), "/ext_dset", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Dflush(dset.get()), 0);

    // Dump object header debug output including the EFL message.
    const H5O_loc_t* dset_loc = H5O_get_loc(dset.get());
    ASSERT_NE(dset_loc, nullptr);
    src_dset_addr = dset_loc->addr;

    FILE* fp = std::tmpfile();
    ASSERT_NE(fp, nullptr);
    absl::Cleanup close_fp = [fp] { std::fclose(fp); };
    {
      ScopedApiContext ctx;
      ASSERT_GE(H5O_debug(dset_loc->file, src_dset_addr, fp, 0, 20), 0);
    }
    const std::string dump = ReadTmpFile(fp);
    EXPECT_THAT(dump, HasSubstr("Heap address:"));
    EXPECT_THAT(dump, HasSubstr("Slots used/allocated:"));
    EXPECT_THAT(dump, HasSubstr("File 0:"));
    EXPECT_THAT(dump, HasSubstr("File 1:"));
    EXPECT_THAT(dump, HasSubstr("ext0.dat"));
    EXPECT_THAT(dump, HasSubstr("ext1.dat"));

    // Also exercise H5O_debug_id directly with small fwidth to hit MAX(..., 0).
    H5O_efl_t efl_msg{};
    {
      ScopedApiContext ctx;
      ASSERT_NE(H5O_msg_read(dset_loc, H5O_EFL_ID, &efl_msg), nullptr);
    }
    absl::Cleanup reset_efl = [&efl_msg] {
      H5O_msg_reset(H5O_EFL_ID, &efl_msg);
    };
    FILE* fp_narrow = std::tmpfile();
    ASSERT_NE(fp_narrow, nullptr);
    absl::Cleanup close_narrow = [fp_narrow] { std::fclose(fp_narrow); };
    {
      ScopedApiContext ctx;
      ASSERT_GE(
          H5O_debug_id(H5O_EFL_ID, dset_loc->file, &efl_msg, fp_narrow, 2, 1),
          0);
    }
    const std::string narrow_dump = ReadTmpFile(fp_narrow);
    EXPECT_THAT(narrow_dump, HasSubstr("Offset of data in file:"));
    EXPECT_THAT(narrow_dump, HasSubstr("Bytes reserved for data:"));
  }

  // Reopen src read-only to exercise decoding from disk, then copy to dst file.
  {
    ScopedFile src(H5Fopen(src_h5.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(src.valid());
    ScopedFile dst(H5Fcreate(dst_h5.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                             H5P_DEFAULT));
    ASSERT_TRUE(dst.valid());

    // H5Ocopy triggers H5O__efl_copy_file into destination file's local heap.
    ASSERT_GE(H5Ocopy(src.get(), "/ext_dset", dst.get(), "/copied_ext_dset",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);

    // Exercise H5O__efl_copy_file with nalloc == 0 via the message class.
    H5F_t* f_src = UnwrapFile(src.get());
    H5F_t* f_dst = UnwrapFile(dst.get());
    ASSERT_NE(f_src, nullptr);
    ASSERT_NE(f_dst, nullptr);
    H5O_efl_t empty_src{};
    bool recompute_size = false;
    {
      ScopedApiContext ctx;
      H5O_efl_t* empty_dst = static_cast<H5O_efl_t*>(
          (H5O_MSG_EFL->copy_file)(f_src, &empty_src, f_dst, &recompute_size,
                                   nullptr, nullptr, nullptr));
      ASSERT_NE(empty_dst, nullptr);
      EXPECT_EQ(empty_dst->nalloc, 0);
      EXPECT_EQ(empty_dst->slot, nullptr);
      H5O_msg_free(H5O_EFL_ID, empty_dst);
    }

    // Read back the copied external dataset to verify heap/slots survived copy.
    ScopedDataset copied(H5Dopen2(dst.get(), "/copied_ext_dset", H5P_DEFAULT));
    ASSERT_TRUE(copied.valid());
    ScopedPlist dcpl(H5Dget_create_plist(copied.get()));
    ASSERT_TRUE(dcpl.valid());
    EXPECT_EQ(H5Pget_external_count(dcpl.get()), 2);

    std::vector<int32_t> read_vals(8, 0);
    ASSERT_GE(H5Dread(copied.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, read_vals.data()),
              0);
    EXPECT_THAT(read_vals,
                ::testing::ElementsAre(10, 20, 30, 40, 50, 60, 70, 80));
  }
}

}  // namespace
}  // namespace hdf5
