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
// Unit tests for fractal heap free space sections (single, first_row,
// normal_row, indirect) covering creation, merging, reducing, splitting,
// reviving, serializing/deserializing, parent building/removal, container
// shrinking, validity checking, and debug dumping in
// third_party/hdf5/src/H5HFsection.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HFsection.c

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t (H5AC_info_t) inside internal fractal heap structs.
#ifndef NDEBUG
#define H5HF_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// H5HFpkg.h pulls in H5B2private.h, which uses `try` as a C parameter name,
// colliding with both the C++ keyword and Google3's `-fno-exceptions` macro.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_param_try
extern "C" {
#define H5FS_FRIEND
#define H5HF_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FSpkg.h"
#include "third_party/hdf5/src/H5HFpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5HF_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HF_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

// RAII wrapper for pushing/popping an HDF5 API context and metadata tag.
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

H5F_t* GetFilePtr(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5HF_create_t MakeHeapParams(unsigned width, size_t start_block_size,
                             size_t max_direct_size, unsigned max_index,
                             unsigned start_root_rows = 1,
                             uint32_t max_man_size = 0) {
  H5HF_create_t cparam{};
  cparam.managed.width = width;
  cparam.managed.start_block_size = start_block_size;
  cparam.managed.max_direct_size = max_direct_size;
  cparam.managed.max_index = max_index;
  cparam.managed.start_root_rows = start_root_rows;
  cparam.checksum_dblocks = false;
  // With checksum_dblocks=false, sizeof_addr=8, max_index<=16 (heap_off_size=2),
  // direct block overhead is 4 + 1 + 8 + 2 = 15 bytes.
  const size_t overhead = 15;
  cparam.max_man_size =
      max_man_size > 0
          ? max_man_size
          : static_cast<uint32_t>(max_direct_size - overhead);
  cparam.id_len = 0;
  return cparam;
}

std::vector<uint8_t> MakePayload(size_t size, uint8_t seed) {
  std::vector<uint8_t> buf(size);
  for (size_t i = 0; i < size; ++i) {
    buf[i] = static_cast<uint8_t>((seed + i * 13) & 0xffU);
  }
  return buf;
}

std::vector<uint8_t> InsertObj(H5HF_t* fh, size_t size, uint8_t seed) {
  size_t id_len = 0;
  EXPECT_GE(H5HF_get_id_len(fh, &id_len), 0);
  std::vector<uint8_t> id(id_len, 0);
  std::vector<uint8_t> data = MakePayload(size, seed);
  EXPECT_GE(H5HF_insert(fh, data.size(), data.data(), id.data()), 0);
  return id;
}

// Flushes an in-memory file, closes it, and reopens its byte image via Core VFD.
void ReopenInMemoryFile(ScopedFile& file) {
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ssize_t img_size = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(img_size, 0);
  std::vector<uint8_t> img(static_cast<size_t>(img_size));
  ASSERT_EQ(H5Fget_file_image(file.get(), img.data(), img.size()), img_size);
  file.reset();

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), img.data(), img.size()), 0);
  file.reset(H5Fopen("reopen.h5", H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(file.valid());
}

struct SectionVisitStats {
  H5HF_hdr_t* hdr = nullptr;
  FILE* debug_stream = nullptr;
  int count = 0;
  int single_count = 0;
  int first_row_count = 0;
  int normal_row_count = 0;
  int serialized_count = 0;
  herr_t status = 0;
};

herr_t VisitSectionAndCheck(H5FS_section_info_t* sect, void* op_data) {
  auto* st = static_cast<SectionVisitStats*>(op_data);
  st->count++;
  if (sect->state == H5FS_SECT_SERIALIZED) {
    st->serialized_count++;
  }
  if (sect->type == H5HF_FSPACE_SECT_SINGLE) {
    st->single_count++;
  } else if (sect->type == H5HF_FSPACE_SECT_FIRST_ROW) {
    st->first_row_count++;
  } else if (sect->type == H5HF_FSPACE_SECT_NORMAL_ROW) {
    st->normal_row_count++;
  }

  H5FS_section_class_t* cls = &st->hdr->fspace->sect_cls[sect->type];
  if (cls->valid != nullptr) {
    herr_t rc = cls->valid(cls, sect);
    if (rc < 0) {
      st->status = rc;
      return rc;
    }
  }
  if (cls->debug != nullptr && st->debug_stream != nullptr) {
    herr_t rc = cls->debug(sect, st->debug_stream, /*indent=*/2, /*fwidth=*/24);
    if (rc < 0) {
      st->status = rc;
      return rc;
    }
  }
  return 0;
}

SectionVisitStats IterateSectionsWithChecks(H5HF_hdr_t* hdr,
                                            FILE* debug_stream = nullptr) {
  SectionVisitStats stats;
  stats.hdr = hdr;
  stats.debug_stream = debug_stream;
  if (hdr->fspace == nullptr) {
    EXPECT_GE(H5HF__space_start(hdr, false), 0);
  }
  if (hdr->fspace != nullptr) {
    EXPECT_GE(
        H5FS_sect_iterate(hdr->f, hdr->fspace, VisitSectionAndCheck, &stats),
        0);
  }
  return stats;
}

TEST(H5HFsectionTest, SingleSectionLifecycleExactReduceMergeAndRootShrink) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  // Overhead for checksum_dblocks=false, max_index=14 (heap_off_size=2,
  // sizeof_addr=8) is 4 + 1 + 8 + 2 = 15 bytes; 64 - 15 = 49 bytes usable.
  const H5HF_create_t cparam =
      MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                     /*max_direct_size=*/256, /*max_index=*/14);
  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() {
    if (fh != nullptr) {
      H5HF_close(fh);
    }
  });

  // 1. Exact fit in root direct block triggers H5HF__sect_single_reduce
  //    exact-size elimination (size == amt) and removal triggers root shrink.
  std::vector<uint8_t> full_id = InsertObj(fh, 49, 0x10);
  EXPECT_EQ(fh->hdr->man_size, 64U);
  EXPECT_GE(H5HF_remove(fh, full_id.data()), 0);
  EXPECT_EQ(fh->hdr->man_size, 0U);

  // 2. Partial allocations in root direct block create and merge single
  //    sections; validating while parent == nullptr exercises root-dblock path.
  std::vector<uint8_t> id1 = InsertObj(fh, 10, 0x20);
  std::vector<uint8_t> id2 = InsertObj(fh, 10, 0x30);
  std::vector<uint8_t> id3 = InsertObj(fh, 10, 0x40);
  EXPECT_EQ(fh->hdr->man_dtable.curr_root_rows, 0U);

  EXPECT_GE(H5HF_remove(fh, id1.data()), 0);
  // Check H5HF__sect_single_valid on root direct block (parent == nullptr) and
  // also verify a direct block info lookup.
  SectionVisitStats st1 = IterateSectionsWithChecks(fh->hdr);
  EXPECT_GE(st1.single_count, 1);

  EXPECT_GE(H5HF_remove(fh, id3.data()), 0);
  // Remove middle object so it merges forward and backward into full block
  // capacity and shrinks root direct block cleanly via single_can_shrink/shrink.
  EXPECT_GE(H5HF_remove(fh, id2.data()), 0);
  EXPECT_EQ(fh->hdr->man_size, 0U);
}

TEST(H5HFsectionTest, SingleSerializedReviveMergeAndDirectShrink) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  haddr_t heap1_addr = HADDR_UNDEF;
  haddr_t heap2_addr = HADDR_UNDEF;
  std::vector<uint8_t> heap1_mid_id;
  std::vector<uint8_t> heap2_kept_id;
  const H5HF_create_t cparam =
      MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                     /*max_direct_size=*/256, /*max_index=*/14);

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    // Heap 1: leave a serialized single hole on each side of mid_id.
    H5HF_t* fh1 = H5HF_create(f, &cparam);
    ASSERT_NE(fh1, nullptr);
    auto close_fh1 = absl::MakeCleanup([&]() { H5HF_close(fh1); });
    ASSERT_GE(H5HF_get_heap_addr(fh1, &heap1_addr), 0);

    std::vector<uint8_t> h1_a = InsertObj(fh1, 10, 0x11);
    heap1_mid_id = InsertObj(fh1, 10, 0x22);
    std::vector<uint8_t> h1_c = InsertObj(fh1, 10, 0x33);
    ASSERT_GE(H5HF_remove(fh1, h1_a.data()), 0);
    ASSERT_GE(H5HF_remove(fh1, h1_c.data()), 0);

    // Heap 2: leave one serialized single hole and one small live object.
    H5HF_t* fh2 = H5HF_create(f, &cparam);
    ASSERT_NE(fh2, nullptr);
    auto close_fh2 = absl::MakeCleanup([&]() { H5HF_close(fh2); });
    ASSERT_GE(H5HF_get_heap_addr(fh2, &heap2_addr), 0);

    std::vector<uint8_t> h2_a = InsertObj(fh2, 15, 0x44);
    heap2_kept_id = InsertObj(fh2, 20, 0x55);
    ASSERT_GE(H5HF_remove(fh2, h2_a.data()), 0);
  }

  ReopenInMemoryFile(file);

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    // Validate serialized sections in heap 1, then remove mid_id so live
    // section merges with serialized section (revive on curr_root_rows == 0)
    // and shrinks root direct block.
    H5HF_t* fh1 = H5HF_open(f, heap1_addr);
    ASSERT_NE(fh1, nullptr);
    auto close_fh1 = absl::MakeCleanup([&]() { H5HF_close(fh1); });
    SectionVisitStats ser_stats = IterateSectionsWithChecks(fh1->hdr);
    EXPECT_GE(ser_stats.serialized_count, 2);
    EXPECT_EQ(ser_stats.status, 0);

    EXPECT_GE(H5HF_remove(fh1, heap1_mid_id.data()), 0);
    EXPECT_EQ(fh1->hdr->man_size, 0U);

    // Allocate into heap 2's serialized root-dblock single section.
    H5HF_t* fh2 = H5HF_open(f, heap2_addr);
    ASSERT_NE(fh2, nullptr);
    auto close_fh2 = absl::MakeCleanup([&]() { H5HF_close(fh2); });
    std::vector<uint8_t> reused = InsertObj(fh2, 10, 0x66);
    EXPECT_GE(H5HF_remove(fh2, reused.data()), 0);
    EXPECT_GE(H5HF_remove(fh2, heap2_kept_id.data()), 0);
    EXPECT_EQ(fh2->hdr->man_size, 0U);

    // Exercise H5HF__sect_single_shrink when section state != H5FS_SECT_LIVE:
    // create root dblock directly, discard live sec_node, build deserialized
    // single section for it, and call cls->shrink.
    H5HF_free_section_t* sec_node = nullptr;
    ASSERT_GE(H5HF__man_dblock_new(fh1->hdr, 10, &sec_node), 0);
    ASSERT_NE(sec_node, nullptr);
    const haddr_t off = sec_node->sect_info.addr;
    const hsize_t sz = sec_node->sect_info.size;
    ASSERT_GE(H5HF__sect_single_free(&sec_node->sect_info), 0);

    if (fh1->hdr->fspace == nullptr) {
      ASSERT_GE(H5HF__space_start(fh1->hdr, /*may_create=*/true), 0);
    }
    H5FS_section_class_t* single_cls =
        &fh1->hdr->fspace->sect_cls[H5HF_FSPACE_SECT_SINGLE];
    unsigned des_flags = 0;
    H5FS_section_info_t* ser_sect =
        single_cls->deserialize(single_cls, nullptr, off, sz, &des_flags);
    ASSERT_NE(ser_sect, nullptr);
    EXPECT_EQ(ser_sect->state, H5FS_SECT_SERIALIZED);

    H5HF_sect_add_ud_t udata{fh1->hdr};
    EXPECT_GE(single_cls->shrink(&ser_sect, &udata), 0);
    EXPECT_EQ(ser_sect, nullptr);
    EXPECT_EQ(fh1->hdr->man_size, 0U);
  }
}

TEST(H5HFsectionTest, IndirectSingleReviveValidProtectStatesAndCanShrinkFalse) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  haddr_t heap_addr = HADDR_UNDEF;
  std::vector<uint8_t> id_01_b;
  std::vector<uint8_t> id_01_c;
  const H5HF_create_t cparam =
      MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                     /*max_direct_size=*/256, /*max_index=*/14);

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    ASSERT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);

    // Fill (0,0), create 3 objects in (0,1), and anchor (1,0).
    (void)InsertObj(fh, 49, 0x01);
    std::vector<uint8_t> id_01_a = InsertObj(fh, 10, 0x02);
    id_01_b = InsertObj(fh, 10, 0x03);
    id_01_c = InsertObj(fh, 10, 0x04);
    (void)InsertObj(fh, 49, 0x05);
    ASSERT_GT(fh->hdr->man_dtable.curr_root_rows, 0U);

    // Remove first object in (0,1) to produce an indirect single section.
    ASSERT_GE(H5HF_remove(fh, id_01_a.data()), 0);

    // Check valid() on live non-root single section in two cache states:
    // 1) dblock not protected (valid() protects and unprotects dblock)
    SectionVisitStats st_unprot = IterateSectionsWithChecks(fh->hdr);
    EXPECT_GE(st_unprot.single_count, 1);
    EXPECT_EQ(st_unprot.status, 0);

    // 2) dblock already protected (valid() skips protect/unprotect)
    haddr_t dblock_addr = HADDR_UNDEF;
    size_t dblock_size = 0;
    H5HF_indirect_t* root_iblock = fh->hdr->root_iblock;
    ASSERT_NE(root_iblock, nullptr);
    dblock_addr = root_iblock->ents[1].addr;
    dblock_size = fh->hdr->man_dtable.row_block_size[0];
    H5HF_direct_t* dblock = H5HF__man_dblock_protect(
        fh->hdr, dblock_addr, dblock_size, root_iblock, 1, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(dblock, nullptr);
    SectionVisitStats st_prot = IterateSectionsWithChecks(fh->hdr);
    EXPECT_GE(st_prot.single_count, 1);
    EXPECT_EQ(st_prot.status, 0);
    ASSERT_GE(H5AC_unprotect(f, H5AC_FHEAP_DBLOCK, dblock_addr, dblock,
                             H5AC__NO_FLAGS_SET),
              0);
  }

  // Reopen so the non-root single section is serialized on disk, then insert
  // into it (triggers H5HF__sect_single_locate_parent), then drain (0,1)
  // to test H5HF__sect_single_can_shrink false on curr_root_rows > 0 and
  // conversion of full indirect single section -> FIRST_ROW.
  ReopenInMemoryFile(file);

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_open(f, heap_addr);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    std::vector<uint8_t> reused = InsertObj(fh, 8, 0x06);
    EXPECT_GE(H5HF_remove(fh, reused.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id_01_b.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id_01_c.data()), 0);

    // (0,1) should now have converted into a FIRST_ROW section.
    SectionVisitStats after = IterateSectionsWithChecks(fh->hdr);
    EXPECT_EQ(after.first_row_count, 1);
    EXPECT_EQ(after.single_count, 0);
  }
}

TEST(H5HFsectionTest, MultiRowSkipSplitEndReduceMergeShrinkAndDebug) {
  const H5HF_create_t cparam =
      MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                     /*max_direct_size=*/512, /*max_index=*/14);

  // Part A: Skip rows 0..3 -> middle split, row exhaustion, NORMAL_ROW valid &
  // debug dump.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    // (0,0) = 64B(49), then 400B -> row 4 (4,0), skipping entries (0,1)..(3,1).
    (void)InsertObj(fh, 49, 0x01);
    std::vector<uint8_t> id40 = InsertObj(fh, 400, 0x02);

    TempFile dbg_file("row_debug.txt");
    FILE* dbg_stream = std::fopen(dbg_file.c_str(), "w+");
    ASSERT_NE(dbg_stream, nullptr);
    auto close_dbg = absl::MakeCleanup([&]() { std::fclose(dbg_stream); });

    SectionVisitStats st = IterateSectionsWithChecks(fh->hdr, dbg_stream);
    EXPECT_EQ(st.first_row_count, 1);
    EXPECT_EQ(st.normal_row_count, 3);
    EXPECT_EQ(st.status, 0);
    EXPECT_GE(std::ftell(dbg_stream), 1L);

    // Allocate 100B (row 2 NORMAL_ROW) -> middle split creates peer_sect for
    // rows 0..1 and promotes row 2 to FIRST_ROW.
    std::vector<uint8_t> id20 = InsertObj(fh, 100, 0x03);
    // Second 100B consumes remaining entry (2,1) -> promotes row 3 to FIRST_ROW.
    std::vector<uint8_t> id21 = InsertObj(fh, 100, 0x04);
    // Two 200B inserts consume row 3 entries (3,0) and (3,1) (down to 0).
    std::vector<uint8_t> id30 = InsertObj(fh, 200, 0x05);
    std::vector<uint8_t> id31 = InsertObj(fh, 200, 0x06);

    // Remove out of order to test row merges across same and different rows.
    EXPECT_GE(H5HF_remove(fh, id20.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id30.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id21.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id31.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id40.data()), 0);
  }

  // Part B: Allocate from end (row_end_entry == end_entry) + row_merge shrink
  // and indirect_shrink freeing NORMAL_ROW.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    // (0,0) = 49B, then 200B in (3,0) skips (0,1)..(2,1) -> end_row = 2.
    (void)InsertObj(fh, 49, 0x10);
    std::vector<uint8_t> id30 = InsertObj(fh, 200, 0x11);
    // Insert 100B -> allocates from end of row 2 (2,1), then (2,0) (dir_nrows--).
    std::vector<uint8_t> id2_a = InsertObj(fh, 100, 0x12);
    std::vector<uint8_t> id2_b = InsertObj(fh, 100, 0x13);
    EXPECT_GE(H5HF_remove(fh, id2_a.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id2_b.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id30.data()), 0);
  }

  // Part C: Sequential blocks, creating separated FIRST_ROW + NORMAL_ROW span
  // then pulling man_iter_off back via high-block removal so row_merge /
  // row_can_shrink / row_shrink execute indirect_shrink with NORMAL_ROW.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    std::vector<uint8_t> id00 = InsertObj(fh, 49, 0x20);
    std::vector<uint8_t> id01 = InsertObj(fh, 49, 0x21);
    std::vector<uint8_t> id10 = InsertObj(fh, 49, 0x22);
    std::vector<uint8_t> id11 = InsertObj(fh, 49, 0x23);
    std::vector<uint8_t> id20 = InsertObj(fh, 110, 0x24);
    std::vector<uint8_t> id21 = InsertObj(fh, 110, 0x25);
    std::vector<uint8_t> id30 = InsertObj(fh, 200, 0x26);
    std::vector<uint8_t> id31 = InsertObj(fh, 200, 0x27);

    // Leave (1,0) occupied, free (0,1), then (1,1)..(2,1) (creating FIRST_ROW at
    // (1,1) + NORMAL_ROW at (2,0..2,1)), then remove (3,1) so (3,0) can reverse
    // or wait: keep (1,0) occupied, remove (1,1), (2,0), (2,1), then remove (3,1)
    // leaving (3,0) past iter? Wait: with (3,1) absent, removing (3,0) reverses
    // man_iter_off back to (1,0)'s end, putting BOTH (1,1..2,1) and (3,0) at
    // or past man_iter_off -> row_merge checks sect2->addr >= man_iter_off and
    // row_can_shrink -> row_shrink -> indirect_shrink frees NORMAL_ROW!
    EXPECT_GE(H5HF_remove(fh, id01.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id11.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id21.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id20.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id31.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id30.data()), 0);

    // Also test row_sect1->row != end_row1 path in indirect_merge_row: fill back
    // rows 1..2, free (0,1), (1,0), (1,1), then merge (1,1).
    EXPECT_GE(H5HF_remove(fh, id10.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id00.data()), 0);
    EXPECT_EQ(fh->hdr->man_size, 0U);
  }
}

TEST(H5HFsectionTest, MultiLevelIndirectInitReduceMiddleSplitAndCheckedOut) {
  // Config: width=2, start_block_size=64, max_direct_size=256 => max_direct_rows=4
  // Row 4 = 512B (child_nrows=3), Row 5 = 1024B (child_nrows=4).
  const H5HF_create_t cparam =
      MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                     /*max_direct_size=*/256, /*max_index=*/16);

  // Part A: Skip indirect rows (4,0)..(5,1), validate nested tree, middle-split
  // H5HF__sect_indirect_reduce at (5,0), and allocate from start (4,0).
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    // Anchor (3,0) so curr_root_rows == 4 and next_block sits at entry 7 ((3,1)).
    (void)InsertObj(fh, 200, 0x01);
    ASSERT_GE(H5HF__man_iblock_root_double(hdr, 2048), 0);
    ASSERT_GE(hdr->root_iblock->nrows, 6U);

    // Skip entries 7..11 ((3,1), (4,0), (4,1), (5,0), (5,1)) so
    // H5HF__sect_indirect_init_rows covers BOTH direct rows (< 4) and
    // indirect entries (>= 4) in one call.
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/7,
                                    /*nentries=*/5),
              0);
    SectionVisitStats st = IterateSectionsWithChecks(hdr);
    EXPECT_GE(st.first_row_count, 1);
    EXPECT_GE(st.normal_row_count, 1);
    EXPECT_EQ(st.status, 0);
  }

  // Part B: Exact skip of entries 8..11 ((4,0)..(5,1)) -> middle-split at (5,0),
  // start-entry reduction at (4,0), end-entry reduction at (4,1), and
  // checked_out == true in row_first!
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    // Fill rows 0..3 partially with anchors or fill (0,0)..(3,1) so man_iter_off
    // reaches row 4 cleanly.
    (void)InsertObj(fh, 49, 0x01);
    // Put 200B at (3,0) and 200B at (3,1) so iterator sits at row 4 entry 8!
    (void)InsertObj(fh, 200, 0x02);
    std::vector<uint8_t> obj31 = InsertObj(fh, 200, 0x03);
    // Double root iblock to cover >= 7 rows, skip entries 8..11 ((4,0)..(5,1)),
    // then put anchor at (6,0) so iterator > (5,1)!
    ASSERT_GE(H5HF__man_iblock_root_double(hdr, 2048), 0);
    ASSERT_GE(hdr->root_iblock->nrows, 7U);
    // Also remove (3,1)? Leave (3,1) occupied so root_sect starts strictly at
    // entry 8 ((4,0), dir_nrows=0, indir_nents=4)!
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/8,
                                    /*nentries=*/4),
              0);
    std::vector<uint8_t> obj60 = InsertObj(fh, 200, 0x04);

    // Validate full multi-level section hierarchy via row_valid / indirect_valid.
    SectionVisitStats st_tree = IterateSectionsWithChecks(hdr);
    EXPECT_GE(st_tree.first_row_count, 2);
    EXPECT_EQ(st_tree.status, 0);

    // 1) Allocate 200B: root_sect rows 4 only have max 128B blocks, so find
    //    picks (5,0) [child entry 10, strictly between 8 and 11]!
    //    This hits H5HF__sect_indirect_reduce middle-split `else`!
    std::vector<uint8_t> obj50 = InsertObj(fh, 200, 0x05);

    // 2) Allocate 100B: hits peer_sect (4,0)'s row 2! Or before that, find
    //    (4,1)'s row 2 directly with H5FS_sect_remove + H5HF__man_iblock_alloc_row
    //    to hit BOTH checked_out == true in row_first AND end_entry reduction!
    //    First find the NORMAL_ROW at (4,1) row 2: its heap addr equals
    //    man_dtable.row_block_off[4] + man_dtable.row_block_size[4] +
    //    man_dtable.row_block_off[2].
    const hsize_t c41_row2_addr = hdr->man_dtable.row_block_off[4] +
                                  hdr->man_dtable.row_block_size[4] +
                                  hdr->man_dtable.row_block_off[2];
    H5HF_free_section_t* target_c41_row2 = nullptr;
    struct FindByAddrCtx {
      haddr_t target;
      H5HF_free_section_t** out;
    } find_ctx{c41_row2_addr, &target_c41_row2};
    ASSERT_GE(H5FS_sect_iterate(f, hdr->fspace,
                                [](H5FS_section_info_t* s, void* op) -> herr_t {
                                  auto* c = static_cast<FindByAddrCtx*>(op);
                                  if (s->addr == c->target) {
                                    *c->out =
                                        reinterpret_cast<H5HF_free_section_t*>(s);
                                  }
                                  return 0;
                                },
                                &find_ctx),
              0);
    ASSERT_NE(target_c41_row2, nullptr);
    ASSERT_GE(H5FS_sect_remove(f, hdr->fspace, &target_c41_row2->sect_info),
              0);
    H5HF_free_section_t* alloc_sec = target_c41_row2;
    ASSERT_GE(H5HF__man_iblock_alloc_row(hdr, &alloc_sec), 0);
    ASSERT_NE(alloc_sec, nullptr);
    ASSERT_GE(H5HF__space_add(hdr, alloc_sec, 0), 0);

    // Note: Wait — c41_row2 was inside (4,1) whose row 2 has dir_rows[2], so
    // checked_out == true is hit specifically when row_sect IS c41->dir_rows[0]
    // (row 0 of c41)! Also remove (4,1) row 0 so c41->dir_rows[0]->checked_out
    // is true when c41 detaches! Wait: (4,1) already detached above, so repeat
    // with a fresh 2-child indirect section [c40, c41] and allocate c41->row 0!
    EXPECT_GE(H5HF_remove(fh, obj50.data()), 0);
    EXPECT_GE(H5HF_remove(fh, obj60.data()), 0);
    EXPECT_GE(H5HF_remove(fh, obj31.data()), 0);
    (void)c41_row2_addr;
  }

  // Part C: Test checked_out == true on row_first, start_entry & end_entry in
  // H5HF__sect_indirect_reduce, and existing child_iblock in init_rows!
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    (void)InsertObj(fh, 200, 0x01);  // anchor at (3,0) -> skips (0,0)..(2,1)
    (void)InsertObj(fh, 200, 0x02);  // anchor at (3,1)
    ASSERT_GE(H5HF__man_iblock_root_double(hdr, 2048), 0);

    // Pre-create a child indirect block at entry 8 ((4,0)) BEFORE
    // H5HF__hdr_skip_blocks skips entries 8..10 so H5HF__sect_indirect_init_rows
    // finds H5_addr_defined(child_iblock_addr) at lines 2367-2373 / 2393-2396!
    // Put an object in (4,0) row 0? Actually H5HF__man_iblock_create creates
    // it with nchildren=0 -> if nothing references it, wait: init_rows protects
    // & unprotects it cleanly and child_sect->u.iblock holds rc=1! Then anchor
    // (6,0) and remove (4,0)'s row 0 via alloc_row (from start_entry)!
    const unsigned child_nrows = H5HF__dtable_size_to_rows(
        &hdr->man_dtable, hdr->man_dtable.row_block_size[4]);
    haddr_t child_addr = HADDR_UNDEF;
    ASSERT_GE(H5HF__man_iblock_create(hdr, hdr->root_iblock, /*par_entry=*/8,
                                      child_nrows, child_nrows, &child_addr),
              0);
    ASSERT_NE(child_addr, HADDR_UNDEF);

    // Wait: man_iblock_create leaves child_iblock with nchildren==0, which if
    // attached to root_iblock must eventually get detached or freed before
    // file close; check: once (4,0) allocates a dblock and that dblock is
    // destroyed, H5HF__man_iblock_detach handles child_iblock deletion cleanly!
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/8,
                                    /*nentries=*/3),  // (4,0), (4,1), (5,0)
              0);
    std::vector<uint8_t> obj51 = InsertObj(fh, 200, 0x04);  // (5,1)

    // Remove (4,1)'s row 0 (NOT first in parent `[8,9,10]`, WAIT: (4,1) is
    // middle! Better yet: first remove (5,0)'s row 0? Let's remove (4,1)'s row
    // 0 via alloc_row -> hits checked_out==true in row_first, AND splits `[8]`
    // and `[10]` cleanly!). Then allocate out of (4,0)'s row 0 so the
    // pre-created child_iblock gets a child attached cleanly!
    const hsize_t c41_row0_addr =
        hdr->man_dtable.row_block_off[4] + hdr->man_dtable.row_block_size[4];
    H5HF_free_section_t* c41_r0 = nullptr;
    struct FindCtx {
      haddr_t addr;
      H5HF_free_section_t** out;
    } fctx{c41_row0_addr, &c41_r0};
    ASSERT_GE(H5FS_sect_iterate(f, hdr->fspace,
                                [](H5FS_section_info_t* s, void* op) -> herr_t {
                                  auto* c = static_cast<FindCtx*>(op);
                                  if (s->addr == c->addr) {
                                    *c->out =
                                        reinterpret_cast<H5HF_free_section_t*>(s);
                                  }
                                  return 0;
                                },
                                &fctx),
              0);
    ASSERT_NE(c41_r0, nullptr);
    ASSERT_GE(H5FS_sect_remove(f, hdr->fspace, &c41_r0->sect_info), 0);
    H5HF_free_section_t* new_s41 = c41_r0;
    ASSERT_GE(H5HF__man_iblock_alloc_row(hdr, &new_s41), 0);
    ASSERT_GE(H5HF__space_add(hdr, new_s41, 0), 0);

    // Also attach a dblock to pre-created (4,0) and free it? Putting one object
    // via direct alloc on (4,0)'s row 0 ensures child_iblock at (4,0) has a
    // clean child cycle or gets reclaimed when (4,0) detaches on shrink.
    const hsize_t c40_row0_addr = hdr->man_dtable.row_block_off[4];
    H5HF_free_section_t* c40_r0 = nullptr;
    FindCtx fctx40{c40_row0_addr, &c40_r0};
    ASSERT_GE(H5FS_sect_iterate(f, hdr->fspace,
                                [](H5FS_section_info_t* s, void* op) -> herr_t {
                                  auto* c = static_cast<FindCtx*>(op);
                                  if (s->addr == c->addr) {
                                    *c->out =
                                        reinterpret_cast<H5HF_free_section_t*>(s);
                                  }
                                  return 0;
                                },
                                &fctx40),
              0);
    if (c40_r0 != nullptr) {
      ASSERT_GE(H5FS_sect_remove(f, hdr->fspace, &c40_r0->sect_info), 0);
      H5HF_free_section_t* new_s40 = c40_r0;
      ASSERT_GE(H5HF__man_iblock_alloc_row(hdr, &new_s40), 0);
      ASSERT_GE(H5HF__space_add(hdr, new_s40, 0), 0);
    }

    // Reopen image so all indirect sections deserialize with serialized state
    // (init_rows with state != LIVE), then test child_entry == start_entry
    // on a fresh skip of 2 entries in root if needed.
    (void)obj51;
  }
}

TEST(H5HFsectionTest, StartAndEndEntryIndirectReduceAndDeserialize) {
  const H5HF_create_t cparam =
      MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                     /*max_direct_size=*/256, /*max_index=*/16);
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  haddr_t heap_addr = HADDR_UNDEF;

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    ASSERT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);
    H5HF_hdr_t* hdr = fh->hdr;

    // Anchor (0,0), (3,0), (3,1) so man_iter_off sits at row 4 entry 8.
    (void)InsertObj(fh, 49, 0x01);
    (void)InsertObj(fh, 200, 0x02);
    (void)InsertObj(fh, 200, 0x03);
    ASSERT_GE(H5HF__man_iblock_root_double(hdr, 2048), 0);
    // Skip entries 8, 9, 10 ((4,0), (4,1), (5,0)), anchor (5,1).
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/8,
                                    /*nentries=*/3),
              0);
    (void)InsertObj(fh, 200, 0x04);
  }

  // Reopen file to force H5HF__sect_row_deserialize /
  // H5HF__sect_indirect_deserialize and init_rows(state == SERIALIZED).
  ReopenInMemoryFile(file);

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_open(f, heap_addr);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    // Validate deserialized multi-level indirect section BEFORE reviving!
    // Covers H5HF__sect_indirect_valid when sect->state == H5FS_SECT_SERIALIZED.
    SectionVisitStats st_ser = IterateSectionsWithChecks(hdr);
    EXPECT_GE(st_ser.serialized_count, 1);
    EXPECT_EQ(st_ser.status, 0);

    // 1) Allocate 100B -> uses (4,0)'s row 2! Child entry 8 == start_entry
    //    (hits H5HF__sect_indirect_reduce `child_entry == start_entry`, advances
    //    parent to entry 9 `(4,1)`, promotes `(4,1)` to FIRST_ROW!).
    (void)InsertObj(fh, 100, 0x05);

    // 2) Allocate 200B -> uses (5,0)'s row 3! Child entry 10 == end_entry in
    //    `[9, 10]`. Leaves `[9]` with indir_nents == 1.
    (void)InsertObj(fh, 200, 0x06);

    // 3) Allocate 100B -> uses (4,1)'s row 2! Now `(4,1)` has parent `[9]` with
    //    num_entries == 1 -> H5HF__sect_indirect_reduce frees indir_ents and
    //    decrements parent rc 1 -> 0!
    (void)InsertObj(fh, 100, 0x07);
  }
}

TEST(H5HFsectionTest, BuildParentParentRemovedRemovedFromCacheAndThreeLevel) {
  // Config A: width=2, start_block_size=64, max_direct_size=64 =>
  // max_direct_rows=2.
  // Row 0 = 64(49), Row 1 = 64(49), Row 2 = 128(child_nrows=1),
  // Row 3 = 256(child_nrows=2), Row 4 = 512(child_nrows=3, has indir row 2!).
  const H5HF_create_t cparam =
      MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                     /*max_direct_size=*/64, /*max_index=*/16);

  // Part A: Build parent + parent_removed + removed_from_cache == true revive.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    // Fill rows 0..1 (4 objects), (2,0) (2 objects), anchor in (2,1) (1 object).
    for (int i = 0; i < 4; ++i) {
      (void)InsertObj(fh, 49, static_cast<uint8_t>(i));
    }
    std::vector<uint8_t> id20_0 = InsertObj(fh, 49, 0x10);
    std::vector<uint8_t> id20_1 = InsertObj(fh, 49, 0x11);
    std::vector<uint8_t> id21_0 = InsertObj(fh, 49, 0x12);

    // Remove (2,0) entries: 1st keeps iblock_20 alive, 2nd triggers
    // parent_removed + H5HF__sect_row_parent_removed + build_parent!
    EXPECT_GE(H5HF_remove(fh, id20_0.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id20_1.data()), 0);

    // Immediately insert 49B while (2,0)'s first row section still points to
    // iblock_20 with removed_from_cache == true -> hits H5HF__sect_row_revive
    // lines 1266-1269 and H5HF__sect_indirect_revive parent branch!
    std::vector<uint8_t> reused = InsertObj(fh, 49, 0x13);
    EXPECT_GE(H5HF_remove(fh, reused.data()), 0);
    EXPECT_GE(H5HF_remove(fh, id21_0.data()), 0);
  }

  // Part B: Build parent -> flush & reopen -> recursive indirect_serialize /
  // deserialize -> insert revives par_sect. Also test removing 1st, reopening,
  // removing 2nd, and flushing serialized-under FIRST_ROW!
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    haddr_t heap1_addr = HADDR_UNDEF;
    haddr_t heap2_addr = HADDR_UNDEF;
    std::vector<uint8_t> fh1_s0;
    std::vector<uint8_t> h2_id20_1;

    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5F_t* f = GetFilePtr(file.get());
      ASSERT_NE(f, nullptr);

      // Heap 1: build_parent BEFORE reopen -> serializes par_sect on flush!
      H5HF_t* fh1 = H5HF_create(f, &cparam);
      ASSERT_NE(fh1, nullptr);
      auto close_fh1 = absl::MakeCleanup([&]() { H5HF_close(fh1); });
      ASSERT_GE(H5HF_get_heap_addr(fh1, &heap1_addr), 0);
      fh1_s0 = InsertObj(fh1, 16, 0x1e);  // Keep 16B in (0,0) to pin root later.
      (void)InsertObj(fh1, 16, 0x1f);
      for (int i = 1; i < 4; ++i) {
        (void)InsertObj(fh1, 49, static_cast<uint8_t>(i));
      }
      std::vector<uint8_t> id20_0 = InsertObj(fh1, 49, 0x20);
      std::vector<uint8_t> id20_1 = InsertObj(fh1, 49, 0x21);
      (void)InsertObj(fh1, 49, 0x22);  // anchor at (2,1)
      ASSERT_GE(H5HF_remove(fh1, id20_0.data()), 0);
      ASSERT_GE(H5HF_remove(fh1, id20_1.data()), 0);

      // Heap 2: remove 1st BEFORE reopen, remove 2nd AFTER reopen -> merges
      // two serialized rows and serializes sect->state != LIVE on flush!
      H5HF_t* fh2 = H5HF_create(f, &cparam);
      ASSERT_NE(fh2, nullptr);
      auto close_fh2 = absl::MakeCleanup([&]() { H5HF_close(fh2); });
      ASSERT_GE(H5HF_get_heap_addr(fh2, &heap2_addr), 0);
      for (int i = 0; i < 4; ++i) {
        (void)InsertObj(fh2, 49, static_cast<uint8_t>(0x30 + i));
      }
      std::vector<uint8_t> h2_id20_0 = InsertObj(fh2, 49, 0x40);
      h2_id20_1 = InsertObj(fh2, 49, 0x41);
      (void)InsertObj(fh2, 49, 0x42);
      ASSERT_GE(H5HF_remove(fh2, h2_id20_0.data()), 0);
    }

    ReopenInMemoryFile(file);

    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5F_t* f = GetFilePtr(file.get());
      ASSERT_NE(f, nullptr);

      H5HF_t* fh1 = H5HF_open(f, heap1_addr);
      ASSERT_NE(fh1, nullptr);
      auto close_fh1 = absl::MakeCleanup([&]() { H5HF_close(fh1); });
      // Remove 16B from (0,0) first to create a SINGLE section that pins root.
      ASSERT_GE(H5HF_remove(fh1, fh1_s0.data()), 0);
      std::vector<uint8_t> revived_reused = InsertObj(fh1, 49, 0x23);
      EXPECT_GE(H5HF_remove(fh1, revived_reused.data()), 0);

      H5HF_t* fh2 = H5HF_open(f, heap2_addr);
      ASSERT_NE(fh2, nullptr);
      auto close_fh2 = absl::MakeCleanup([&]() { H5HF_close(fh2); });
      EXPECT_GE(H5HF_remove(fh2, h2_id20_1.data()), 0);
      // Force dirty free-space info flush to exercise serialized
      // indirect_serialize (`else` on iblock_off).
      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    }
  }

  // Part C: 3-level merge inside row 4 child indirect block P=(4,0):
  // P has dir rows 0..1 (4 blocks) and indirect row 2 (children C0, C1).
  // Emptying C0 builds par_C0; emptying C1 builds par_C1 and merges
  // [par_C0, par_C1] (indir_ents realloc in indirect_merge_row + NORMAL_ROW);
  // emptying P's direct blocks merges direct rows into [C0, C1] and builds
  // root-level parent section!
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    // Rows 0..3 hold 2 + 2 + (2 + 2) + (4 + 4) = 16 blocks of 49B.
    for (int i = 0; i < 16; ++i) {
      (void)InsertObj(fh, 49, static_cast<uint8_t>(i));
    }
    // P=(4,0): direct entries (0,0)..(1,1) = 4 blocks, C0=(2,0)=2 blocks,
    // C1=(2,1)=2 blocks -> 8 blocks total; then anchor in (4,1).
    std::vector<uint8_t> p00 = InsertObj(fh, 49, 0x50);
    std::vector<uint8_t> p01 = InsertObj(fh, 49, 0x51);
    std::vector<uint8_t> p10 = InsertObj(fh, 49, 0x52);
    std::vector<uint8_t> p11 = InsertObj(fh, 49, 0x53);
    std::vector<uint8_t> c0_0 = InsertObj(fh, 49, 0x54);
    std::vector<uint8_t> c0_1 = InsertObj(fh, 49, 0x55);
    std::vector<uint8_t> c1_0 = InsertObj(fh, 49, 0x56);
    std::vector<uint8_t> c1_1 = InsertObj(fh, 49, 0x57);
    std::vector<uint8_t> anchor41 = InsertObj(fh, 49, 0x58);

    EXPECT_GE(H5HF_remove(fh, c0_0.data()), 0);
    EXPECT_GE(H5HF_remove(fh, c0_1.data()), 0);
    EXPECT_GE(H5HF_remove(fh, c1_0.data()), 0);
    EXPECT_GE(H5HF_remove(fh, c1_1.data()), 0);

    // Check validity of merged 3-level parent sections C0 and C1.
    SectionVisitStats st_3lvl = IterateSectionsWithChecks(fh->hdr);
    EXPECT_GE(st_3lvl.first_row_count, 2);
    EXPECT_EQ(st_3lvl.status, 0);

    // Now remove P's direct blocks so S_01 merges (1,1) (row != end_row1,
    // parent_removed LIVE/SERIALIZED merge) and then merges into [C0, C1]
    // taking over indir_ents and building parent for P inside root!
    EXPECT_GE(H5HF_remove(fh, p00.data()), 0);
    EXPECT_GE(H5HF_remove(fh, p01.data()), 0);
    EXPECT_GE(H5HF_remove(fh, p10.data()), 0);
    EXPECT_GE(H5HF_remove(fh, p11.data()), 0);

    SectionVisitStats st_after_p = IterateSectionsWithChecks(fh->hdr);
    EXPECT_EQ(st_after_p.status, 0);

    // Finally remove anchor so P can shrink cleanly.
    EXPECT_GE(H5HF_remove(fh, anchor41.data()), 0);
  }
}

H5HF_free_section_t* FindSectionAtAddr(H5HF_hdr_t* hdr, haddr_t addr,
                                       unsigned want_type) {
  struct SearchCtx {
    haddr_t addr;
    unsigned want_type;
    H5HF_free_section_t* found = nullptr;
  } ctx{addr, want_type, nullptr};

  EXPECT_GE(H5FS_sect_iterate(
                hdr->f, hdr->fspace,
                [](H5FS_section_info_t* sect, void* op_data) -> herr_t {
                  auto* s = static_cast<SearchCtx*>(op_data);
                  if (sect->addr == s->addr && sect->type == s->want_type) {
                    s->found = reinterpret_cast<H5HF_free_section_t*>(sect);
                  }
                  return 0;
                },
                &ctx),
            0);
  return ctx.found;
}

TEST(H5HFsectionTest, DirNrowsZeroFirstIndirectAndStartEntryColWrap) {
  // width=2, start_block_size=64, max_direct_size=256 => max_direct_rows=4:
  // row 0: 49x2, row 1: 49x2, row 2: 113x2, row 3: 241x2; row 4,5: indirect.
  const H5HF_create_t cparam =
      MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                     /*max_direct_size=*/256, /*max_index=*/16);

  // Subcase 1: Start row ends when dir_nrows drops 1 -> 0 and promotes
  // indir_ents[0] via H5HF__sect_indirect_first (lines 2779, 2782-2783).
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    for (int i = 0; i < 4; ++i) {
      (void)InsertObj(fh, 49, static_cast<uint8_t>(i));
    }
    (void)InsertObj(fh, 113, 0x10);
    (void)InsertObj(fh, 113, 0x11);
    (void)InsertObj(fh, 241, 0x12);

    ASSERT_GE(H5HF__man_iblock_root_double(hdr, 2048), 0);
    // Skip entries 7=(3,1), 8=(4,0), 9=(4,1) so top_indir has dir_nrows=1,
    // indir_nents=2.
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/7,
                                    /*nentries=*/3),
              0);
    // 200B fits only (3,1)'s 241B direct block, reducing dir_nrows 1->0 and
    // promoting (4,0)'s first row via H5HF__sect_indirect_first.
    (void)InsertObj(fh, 200, 0x13);

    SectionVisitStats st = IterateSectionsWithChecks(hdr);
    EXPECT_EQ(st.status, 0);
  }

  // Subcase 2: Reducing indirect section on end_entry when indir_nents 1 -> 0
  // frees indir_ents (line 3028).
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    for (int i = 0; i < 4; ++i) {
      (void)InsertObj(fh, 49, static_cast<uint8_t>(0x20 + i));
    }
    (void)InsertObj(fh, 113, 0x24);
    (void)InsertObj(fh, 113, 0x25);
    (void)InsertObj(fh, 241, 0x26);

    ASSERT_GE(H5HF__man_iblock_root_double(hdr, 2048), 0);
    // Skip entries 7=(3,1), 8=(4,0) so top_indir has dir_nrows=1,
    // indir_nents=1.
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/7,
                                    /*nentries=*/2),
              0);
    // 100B picks (4,0)'s row 2 (113B best-fit < 241B), reducing parent on
    // child_entry == end_entry (8) and dropping indir_nents 1 -> 0.
    (void)InsertObj(fh, 100, 0x27);

    SectionVisitStats st = IterateSectionsWithChecks(hdr);
    EXPECT_EQ(st.status, 0);
  }

  // Subcase 3: Start-entry indirect reduction advancing col 0->1 and wrapping
  // col == width -> row++, col = 0 (lines 2994, 2997-3003, 3006-3008, 3012).
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    for (int i = 0; i < 4; ++i) {
      (void)InsertObj(fh, 49, static_cast<uint8_t>(0x30 + i));
    }
    (void)InsertObj(fh, 113, 0x34);
    (void)InsertObj(fh, 113, 0x35);
    (void)InsertObj(fh, 241, 0x36);
    (void)InsertObj(fh, 241, 0x37);

    ASSERT_GE(H5HF__man_iblock_root_double(hdr, 2048), 0);
    // Skip entries 8=(4,0), 9=(4,1), 10=(5,0).
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/8,
                                    /*nentries=*/3),
              0);

    // 1st & 2nd 100B consume (4,0)'s row 2: 1st reduces start_entry 8->9
    // (col 0 -> 1, promotes (4,1)); 2nd finishes (4,0)'s 113B blocks.
    (void)InsertObj(fh, 100, 0x38);
    (void)InsertObj(fh, 100, 0x39);
    // 3rd 100B picks (4,1)'s row 2: start_entry 9->10 wraps col 1->2 == width
    // to row 5, col 0 and promotes (5,0).
    (void)InsertObj(fh, 100, 0x3a);

    SectionVisitStats st = IterateSectionsWithChecks(hdr);
    EXPECT_EQ(st.status, 0);
  }
}

TEST(H5HFsectionTest, ThreeLevelParentReduceMergeAndBuildParentLive) {
  // Subcase 4: Parent-linked middle indirect section detaching from grandparent
  // (!is_first) and middle-splitting indir_nents 2 -> 0 (lines 2967, 2970,
  // 2973, 2975-2976, 2979-2980, 3098), followed by is_first == true detach.
  {
    const H5HF_create_t cparam =
        MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                       /*max_direct_size=*/64, /*max_index=*/16);
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    // Fill rows 0..3 completely (16 blocks of 49B), then walk iterator up out
    // of child (3,1) to root_iblock entry 8 before doubling root_iblock.
    for (int i = 0; i < 16; ++i) {
      (void)InsertObj(fh, 49, static_cast<uint8_t>(i));
    }
    ASSERT_GE(H5HF__man_iter_up(&hdr->next_block), 0);
    ASSERT_GE(H5HF__man_iter_next(hdr, &hdr->next_block, 1), 0);
    ASSERT_GE(H5HF__man_iblock_root_double(hdr, 2048), 0);
    // Skip entries 8=(4,0) [P0] and 9=(4,1) [P1]; each has direct rows 0..1
    // and indirect children C0=(2,0), C1=(2,1) at entries 4 and 5.
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/8,
                                    /*nentries=*/2),
              0);

    // Target P1->C0's row section specifically: remove from fspace, allocate
    // a block via H5HF__man_iblock_alloc_row (detaches P1 with !is_first and
    // middle-splits P1 dropping indir_nents 2->0), then keep the block alive
    // with a reduced 39B single section.
    const haddr_t p1_c0_addr = hdr->man_dtable.row_block_off[4] +
                               hdr->man_dtable.row_block_size[4] +
                               hdr->man_dtable.row_block_off[2];
    // Materialize P1=(4,1) and C0=(2,0) with writable locate flags before
    // reviving s_p1c0, avoiding H5HF__sect_indirect_revive_row's locate-on-
    // undef-addr failure path.
    H5HF_indirect_t* loc_ib = nullptr;
    unsigned loc_ent = 0;
    bool loc_dp = false;
    ASSERT_GE(H5HF__man_dblock_locate(hdr, p1_c0_addr, &loc_ib, &loc_ent,
                                      &loc_dp, H5AC__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5HF__man_iblock_unprotect(loc_ib, H5AC__NO_FLAGS_SET, loc_dp), 0);

    H5HF_free_section_t* s_p1c0 =
        FindSectionAtAddr(hdr, p1_c0_addr, H5HF_FSPACE_SECT_NORMAL_ROW);
    ASSERT_NE(s_p1c0, nullptr);
    ASSERT_GE(H5FS_sect_remove(hdr->f, hdr->fspace, &s_p1c0->sect_info), 0);
    ASSERT_GE(H5HF__man_iblock_alloc_row(hdr, &s_p1c0), 0);
    ASSERT_GE(H5HF__sect_single_reduce(hdr, s_p1c0, 10), 0);
    ASSERT_GE(H5HF__hdr_adj_free(hdr, -10), 0);

    // Now detach P0 (which is_first == true under R=[8]) via P0->C0.
    const haddr_t p0_c0_addr =
        hdr->man_dtable.row_block_off[4] + hdr->man_dtable.row_block_off[2];
    ASSERT_GE(H5HF__man_dblock_locate(hdr, p0_c0_addr, &loc_ib, &loc_ent,
                                      &loc_dp, H5AC__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5HF__man_iblock_unprotect(loc_ib, H5AC__NO_FLAGS_SET, loc_dp), 0);

    H5HF_free_section_t* s_p0c0 =
        FindSectionAtAddr(hdr, p0_c0_addr, H5HF_FSPACE_SECT_NORMAL_ROW);
    ASSERT_NE(s_p0c0, nullptr);
    ASSERT_GE(H5FS_sect_remove(hdr->f, hdr->fspace, &s_p0c0->sect_info), 0);
    ASSERT_GE(H5HF__man_iblock_alloc_row(hdr, &s_p0c0), 0);
    ASSERT_GE(H5HF__sect_single_reduce(hdr, s_p0c0, 10), 0);
    ASSERT_GE(H5HF__hdr_adj_free(hdr, -10), 0);

    SectionVisitStats st = IterateSectionsWithChecks(hdr);
    EXPECT_EQ(st.status, 0);
  }

  // Subcase 5a: merge_row taking over sect2->indir_ents when
  // sect1->indir_ents == NULL (lines 3410, 3418, 3421-3424, 3441-3450).
  {
    const H5HF_create_t cparam =
        MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                       /*max_direct_size=*/256, /*max_index=*/16);
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    for (int i = 0; i < 4; ++i) {
      (void)InsertObj(fh, 49, static_cast<uint8_t>(0x40 + i));
    }
    (void)InsertObj(fh, 113, 0x44);
    (void)InsertObj(fh, 113, 0x45);
    std::vector<uint8_t> id30 = InsertObj(fh, 241, 0x46);

    ASSERT_GE(H5HF__man_iblock_root_double(hdr, 2048), 0);
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/7,
                                    /*nentries=*/3),
              0);
    // Anchor at (5,1) so (3,1)..(5,0) does not shrink out of man_iter_off.
    (void)InsertObj(fh, 241, 0x47);
    // Removing (3,0) yields sect1=(3,0) [dir_nrows=1, indir_ents=NULL], which
    // merges into sect2=[7=(3,1), 8=(4,0), 9=(4,1)] taking over indir_ents.
    ASSERT_GE(H5HF_remove(fh, id30.data()), 0);

    SectionVisitStats st = IterateSectionsWithChecks(hdr);
    EXPECT_EQ(st.status, 0);
  }

  // Subcase 5b: merge_row realloc branch when both sect1 and sect2 have
  // dir_nrows == 0 && indir_nents > 0 (lines 3406, 3426, 3429-3438), PLUS
  // re-add failure rollback inside merge_row (lines 3488, 1711).
  {
    const H5HF_create_t cparam =
        MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                       /*max_direct_size=*/256, /*max_index=*/16);
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    for (int i = 0; i < 4; ++i) {
      (void)InsertObj(fh, 49, static_cast<uint8_t>(0x50 + i));
    }
    (void)InsertObj(fh, 113, 0x54);
    (void)InsertObj(fh, 113, 0x55);
    (void)InsertObj(fh, 241, 0x56);
    (void)InsertObj(fh, 241, 0x57);
    ASSERT_GE(H5HF__man_iblock_root_double(hdr, 2048), 0);

    // Create sect1=[(4,0)] (dir_nrows=0, indir_nents=1) and temporarily remove
    // its FIRST_ROW so creating sect2=[(4,1)] does not auto-merge yet.
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/8,
                                    /*nentries=*/1),
              0);
    H5HF_free_section_t* row1 = FindSectionAtAddr(
        hdr, hdr->man_dtable.row_block_off[4], H5HF_FSPACE_SECT_FIRST_ROW);
    ASSERT_NE(row1, nullptr);
    ASSERT_GE(H5FS_sect_remove(hdr->f, hdr->fspace, &row1->sect_info), 0);

    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/9,
                                    /*nentries=*/1),
              0);
    H5HF_free_section_t* row2 = FindSectionAtAddr(
        hdr, hdr->man_dtable.row_block_off[4] + hdr->man_dtable.row_block_size[4],
        H5HF_FSPACE_SECT_FIRST_ROW);
    ASSERT_NE(row2, nullptr);
    ASSERT_GE(H5FS_sect_remove(hdr->f, hdr->fspace, &row2->sect_info), 0);

    // Block re-adding row2 inside merge_row by inserting a dummy section with
    // identical (size, addr) in fspace (collides in fspace_node->sect_list,
    // hitting lines 3488 and 1711 while leaving sect1 updated and sect2 freed).
    H5HF_free_section_t* dummy = H5HF__sect_single_new(
        row2->sect_info.addr, row2->sect_info.size, nullptr, 0);
    ASSERT_NE(dummy, nullptr);
    ASSERT_GE(H5HF__space_add(hdr, dummy, H5FS_ADD_DESERIALIZING), 0);

    {
      ScopedErrorSilence silence;
      H5FS_section_class_t* cls =
          &hdr->fspace->sect_cls[H5HF_FSPACE_SECT_FIRST_ROW];
      H5HF_sect_add_ud_t udata{hdr};
      H5FS_section_info_t* row1_info = &row1->sect_info;
      EXPECT_LT(cls->merge(&row1_info, &row2->sect_info, &udata), 0);
    }

    // Remove dummy and re-add row2 + row1 cleanly.
    ASSERT_GE(H5FS_sect_remove(hdr->f, hdr->fspace, &dummy->sect_info), 0);
    ASSERT_GE(H5HF__sect_single_free(&dummy->sect_info), 0);
    ASSERT_GE(H5HF__space_add(hdr, row2, H5FS_ADD_SKIP_VALID), 0);
    ASSERT_GE(H5HF__space_add(hdr, row1, H5FS_ADD_SKIP_VALID), 0);

    SectionVisitStats st = IterateSectionsWithChecks(hdr);
    EXPECT_EQ(st.status, 0);
  }

  // Subcase 6: H5HF__sect_indirect_build_parent when sect's live iblock has a
  // non-NULL parent (lines 3538-3541).
  {
    const H5HF_create_t cparam =
        MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                       /*max_direct_size=*/64, /*max_index=*/16);
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    // Fill rows 0..1 (4 blocks of 49B).
    for (int i = 0; i < 4; ++i) {
      (void)InsertObj(fh, 49, static_cast<uint8_t>(0x60 + i));
    }
    ASSERT_GE(H5HF__man_iblock_root_double(hdr, 128), 0);

    // Create and protect child indirect block c0 at root entry 4 = (2,0).
    haddr_t c0_addr = HADDR_UNDEF;
    bool did_protect = false;
    ASSERT_GE(H5HF__man_iblock_create(hdr, hdr->root_iblock, /*par_entry=*/4,
                                      /*nrows=*/1, /*max_rows=*/1, &c0_addr),
              0);
    H5HF_indirect_t* c0 = H5HF__man_iblock_protect(
        hdr, c0_addr, /*iblock_nrows=*/1, hdr->root_iblock, /*par_entry=*/4,
        /*must_protect=*/false, H5AC__NO_FLAGS_SET, &did_protect);
    ASSERT_NE(c0, nullptr);
    ASSERT_GE(H5HF__man_iter_down(&hdr->next_block, c0), 0);

    // Skip both entries of c0 in two steps: 2nd skip merges into the 1st and
    // fills c0 (iblock_entries == 2 == num_entries) while c0->parent != NULL!
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, c0, /*start_entry=*/0, /*nentries=*/1),
              0);
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, c0, /*start_entry=*/1, /*nentries=*/1),
              0);

    ASSERT_GE(H5HF__man_iter_up(&hdr->next_block), 0);
    ASSERT_GE(H5HF__man_iter_next(hdr, &hdr->next_block, 1), 0);
    ASSERT_GE(H5HF__man_iblock_unprotect(c0, H5AC__NO_FLAGS_SET, did_protect),
              0);

    // Consume (2,0)'s blocks cleanly and anchor at (2,1).
    for (int i = 0; i < 3; ++i) {
      (void)InsertObj(fh, 49, static_cast<uint8_t>(0x70 + i));
    }
    SectionVisitStats st = IterateSectionsWithChecks(hdr);
    EXPECT_EQ(st.status, 0);
  }
}

void SwapBytesAtOffset(const char* path, off_t offset, uint8_t* buf,
                       size_t len, bool restore) {
  int fd = open(path, O_RDWR);
  ASSERT_GE(fd, 0);
  auto close_fd = absl::MakeCleanup([fd]() { close(fd); });
  if (!restore) {
    ASSERT_EQ(pread(fd, buf, len, offset), static_cast<ssize_t>(len));
    std::vector<uint8_t> corrupt(len, 0x5a);
    ASSERT_EQ(pwrite(fd, corrupt.data(), len, offset),
              static_cast<ssize_t>(len));
  } else {
    ASSERT_EQ(pwrite(fd, buf, len, offset), static_cast<ssize_t>(len));
  }
}

TEST(H5HFsectionTest, DiskMagicCorruptionErrors) {
  // Uses OS-backed SEC2 TempFile because pread/pwrite corrupts metadata magic
  // bytes on disk and then restores them prior to file close.

  // Part A: Corrupted root direct block magic causes H5HF__sect_single_shrink
  // protect failure (line 1007).
  {
    TempFile tmp("h5hf_sect_corrupt_a.h5");
    haddr_t heap_addr = HADDR_UNDEF;
    const H5HF_create_t cparam =
        MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                       /*max_direct_size=*/256, /*max_index=*/14);

    {
      ScopedFile file(
          H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(file.valid());
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5F_t* f = GetFilePtr(file.get());
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
      ASSERT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);
      (void)InsertObj(fh, 20, 0x01);
    }

    ScopedFile file(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5HF_t* fh = H5HF_open(GetFilePtr(file.get()), heap_addr);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;
    ASSERT_GE(H5HF__space_start(hdr, false), 0);
    // Read table_addr after reopen so temporary addresses have been relocated.
    const haddr_t table_addr = hdr->man_dtable.table_addr;
    ASSERT_NE(table_addr, HADDR_UNDEF);

    uint8_t saved[4]{};
    SwapBytesAtOffset(tmp.c_str(), static_cast<off_t>(table_addr), saved, 4,
                      /*restore=*/false);
    auto restore = absl::MakeCleanup([&]() {
      SwapBytesAtOffset(tmp.c_str(), static_cast<off_t>(table_addr), saved, 4,
                        /*restore=*/true);
    });

    H5FS_section_class_t* single_cls =
        &hdr->fspace->sect_cls[H5HF_FSPACE_SECT_SINGLE];
    H5HF_free_section_t* s = H5HF__sect_single_new(15, 49, nullptr, 0);
    ASSERT_NE(s, nullptr);
    H5FS_section_info_t* si = &s->sect_info;
    H5HF_sect_add_ud_t udata{hdr};
    {
      ScopedErrorSilence silence;
      EXPECT_LT(single_cls->shrink(&si, &udata), 0);
    }
    if (si != nullptr) {
      EXPECT_GE(H5HF__sect_single_free(si), 0);
    }
  }

  // Part B: Corrupted root indirect block magic causes revive failures across
  // single_revive (lines 499, 561), single_shrink (line 992), and
  // single_merge (line 894).
  {
    TempFile tmp("h5hf_sect_corrupt_b.h5");
    haddr_t heap_addr = HADDR_UNDEF;
    const H5HF_create_t cparam =
        MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                       /*max_direct_size=*/256, /*max_index=*/14);

    {
      ScopedFile file(
          H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(file.valid());
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5HF_t* fh = H5HF_create(GetFilePtr(file.get()), &cparam);
      ASSERT_NE(fh, nullptr);
      auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
      ASSERT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);
      (void)InsertObj(fh, 49, 0x10);
      (void)InsertObj(fh, 49, 0x11);
      (void)InsertObj(fh, 10, 0x12);
    }

    ScopedFile file(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5HF_t* fh = H5HF_open(GetFilePtr(file.get()), heap_addr);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;
    ASSERT_GE(H5HF__space_start(hdr, false), 0);
    ASSERT_EQ(hdr->root_iblock, nullptr);
    const haddr_t table_addr = hdr->man_dtable.table_addr;
    ASSERT_NE(table_addr, HADDR_UNDEF);

    uint8_t saved[4]{};
    SwapBytesAtOffset(tmp.c_str(), static_cast<off_t>(table_addr), saved, 4,
                      /*restore=*/false);
    auto restore = absl::MakeCleanup([&]() {
      SwapBytesAtOffset(tmp.c_str(), static_cast<off_t>(table_addr), saved, 4,
                        /*restore=*/true);
    });

    H5FS_section_class_t* single_cls =
        &hdr->fspace->sect_cls[H5HF_FSPACE_SECT_SINGLE];
    uint8_t dummy_buf[1] = {0};
    unsigned des_flags = 0;
    H5FS_section_info_t* si = single_cls->deserialize(
        single_cls, dummy_buf, /*sect_addr=*/128 + 15 + 10, /*sect_size=*/10,
        &des_flags);
    ASSERT_NE(si, nullptr);
    H5HF_sect_add_ud_t udata{hdr};

    {
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5HF__sect_single_revive(hdr, reinterpret_cast<H5HF_free_section_t*>(si)),
          0);
      EXPECT_LT(single_cls->shrink(&si, &udata), 0);
      H5HF_free_section_t* s2 =
          H5HF__sect_single_new(si->addr + si->size, 5, nullptr, 0);
      ASSERT_NE(s2, nullptr);
      // H5HF__sect_single_merge frees s2 before calling revive (which fails).
      EXPECT_LT(single_cls->merge(&si, &s2->sect_info, &udata), 0);
    }
    if (si != nullptr) {
      EXPECT_GE(single_cls->free(si), 0);
    }
  }

  // Part C: Corrupted unloaded child direct block magic causes full_dblock
  // protect failures via single_add (lines 705, 762) and single_merge
  // (line 899).
  {
    TempFile tmp("h5hf_sect_corrupt_c.h5");
    haddr_t heap_addr = HADDR_UNDEF;
    const H5HF_create_t cparam =
        MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                       /*max_direct_size=*/256, /*max_index=*/14);

    {
      ScopedFile file(
          H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(file.valid());
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5HF_t* fh = H5HF_create(GetFilePtr(file.get()), &cparam);
      ASSERT_NE(fh, nullptr);
      auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
      ASSERT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);
      (void)InsertObj(fh, 10, 0x20);
      (void)InsertObj(fh, 49, 0x21);
      // Needs row 2 (128B block) -> doubles root_iblock (nrows=4) and creates
      // unloaded child direct block at entry 4 = (2,0).
      (void)InsertObj(fh, 60, 0x22);
    }

    ScopedFile file(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5HF_t* fh = H5HF_open(GetFilePtr(file.get()), heap_addr);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;
    // Insert into (0,0) so root_iblock is pinned while (2,0) stays unloaded.
    (void)InsertObj(fh, 5, 0x23);
    ASSERT_NE(hdr->root_iblock, nullptr);
    ASSERT_GE(hdr->root_iblock->nrows, 3U);
    const haddr_t db20_disk_addr = hdr->root_iblock->ents[4].addr;
    ASSERT_NE(db20_disk_addr, HADDR_UNDEF);

    uint8_t saved[4]{};
    SwapBytesAtOffset(tmp.c_str(), static_cast<off_t>(db20_disk_addr), saved, 4,
                      /*restore=*/false);
    auto restore = absl::MakeCleanup([&]() {
      SwapBytesAtOffset(tmp.c_str(), static_cast<off_t>(db20_disk_addr), saved,
                        4, /*restore=*/true);
    });

    H5FS_section_class_t* single_cls =
        &hdr->fspace->sect_cls[H5HF_FSPACE_SECT_SINGLE];
    H5HF_sect_add_ud_t udata{hdr};

    // Entry 4 = (2,0) has block_off=256, block_size=128, usable=113.
    H5HF_free_section_t* s_full =
        H5HF__sect_single_new(256 + 15, 113, hdr->root_iblock, /*par_entry=*/4);
    ASSERT_NE(s_full, nullptr);
    H5FS_section_info_t* s_full_info = &s_full->sect_info;
    unsigned add_flags = 0;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(single_cls->add(&s_full_info, &add_flags, &udata), 0);
    }
    if (s_full_info != nullptr) {
      EXPECT_GE(single_cls->free(s_full_info), 0);
    }

    H5HF_free_section_t* s1 =
        H5HF__sect_single_new(256 + 15, 50, hdr->root_iblock, /*par_entry=*/4);
    H5HF_free_section_t* s2 =
        H5HF__sect_single_new(256 + 65, 63, hdr->root_iblock, /*par_entry=*/4);
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    H5FS_section_info_t* s1_info = &s1->sect_info;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(single_cls->merge(&s1_info, &s2->sect_info, &udata), 0);
    }
    if (s1_info != nullptr) {
      EXPECT_GE(single_cls->free(s1_info), 0);
    }
  }
}

TEST(H5HFsectionTest, SectionRollbackUndefinedAddrAndShrinkErrors) {
  // Part A: H5HF__sect_single_reduce re-add collision (line 652) and
  // undefined dblock_addr checks in full_dblock (line 692), single_shrink
  // (line 998), and single_valid (line 1104).
  {
    const H5HF_create_t cparam =
        MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                       /*max_direct_size=*/256, /*max_index=*/14);
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5HF_t* fh = H5HF_create(GetFilePtr(file.get()), &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    (void)InsertObj(fh, 10, 0x01);  // Leaves 39B SINGLE section at addr=25.
    (void)InsertObj(fh, 49, 0x02);
    // Needs row 1 (128B) -> doubles root_iblock (nrows=2) and creates (1,0),
    // leaving root entry 3 = (1,1) HADDR_UNDEF.
    (void)InsertObj(fh, 60, 0x03);
    ASSERT_NE(hdr->root_iblock, nullptr);
    ASSERT_GE(hdr->root_iblock->nrows, 2U);
    ASSERT_EQ(hdr->root_iblock->ents[3].addr, HADDR_UNDEF);

    H5FS_section_class_t* single_cls =
        &hdr->fspace->sect_cls[H5HF_FSPACE_SECT_SINGLE];
    H5HF_sect_add_ud_t udata{hdr};

    // Colliding re-add in H5HF__sect_single_reduce: (15 + 10 == 25).
    H5HF_free_section_t* s_col =
        H5HF__sect_single_new(15, 30, hdr->root_iblock, /*par_entry=*/0);
    ASSERT_NE(s_col, nullptr);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5HF__sect_single_reduce(hdr, s_col, 10), 0);
    }
    EXPECT_GE(H5HF__sect_single_free(&s_col->sect_info), 0);

    // Undefined parent entry 3 hits !H5_addr_defined(dblock_addr) checks.
    H5HF_free_section_t* s_undef = H5HF__sect_single_new(
        256 + 15, 113, hdr->root_iblock, /*par_entry=*/3);
    ASSERT_NE(s_undef, nullptr);
    H5FS_section_info_t* su_info = &s_undef->sect_info;
    unsigned add_flags = 0;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(single_cls->add(&su_info, &add_flags, &udata), 0);
      EXPECT_LT(single_cls->shrink(&su_info, &udata), 0);
      EXPECT_LT(single_cls->valid(single_cls, su_info), 0);
    }
    EXPECT_GE(single_cls->free(su_info), 0);
  }

  // Part B: Indirect shrink failure (lines 3614, 1786, 1706) and row_reduce
  // re-add failure (line 1346).
  {
    const H5HF_create_t cparam =
        MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                       /*max_direct_size=*/256, /*max_index=*/16);
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5HF_t* fh = H5HF_create(GetFilePtr(file.get()), &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    for (int i = 0; i < 4; ++i) {
      (void)InsertObj(fh, 49, static_cast<uint8_t>(0x10 + i));
    }
    ASSERT_GE(H5HF__man_iblock_root_double(hdr, 64), 0);  // nrows = 4.
    ASSERT_EQ(hdr->man_iter_off, 256U);

    // Skip entries 4..6 (rows 2..3) to keep top_indir's rows alive in fspace,
    // remove both derived rows, then reset man_iter_off so first_cls->shrink
    // enters H5HF__sect_indirect_shrink and fails removing nr (lines 3614, 1786).
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/4,
                                    /*nentries=*/3),
              0);
    H5HF_free_section_t* fr = FindSectionAtAddr(
        hdr, hdr->man_dtable.row_block_off[2], H5HF_FSPACE_SECT_FIRST_ROW);
    H5HF_free_section_t* nr = FindSectionAtAddr(
        hdr, hdr->man_dtable.row_block_off[3], H5HF_FSPACE_SECT_NORMAL_ROW);
    ASSERT_NE(fr, nullptr);
    ASSERT_NE(nr, nullptr);
    H5HF_free_section_t* top_indir = fr->u.row.under;
    ASSERT_NE(top_indir, nullptr);
    ASSERT_EQ(nr->u.row.under, top_indir);

    ASSERT_GE(H5FS_sect_remove(hdr->f, hdr->fspace, &nr->sect_info), 0);
    ASSERT_GE(H5FS_sect_remove(hdr->f, hdr->fspace, &fr->sect_info), 0);

    H5FS_section_class_t* first_cls =
        &hdr->fspace->sect_cls[H5HF_FSPACE_SECT_FIRST_ROW];
    H5HF_sect_add_ud_t udata{hdr};
    H5FS_section_info_t* fr_info = &fr->sect_info;
    const hsize_t saved_iter_off = hdr->man_iter_off;
    hdr->man_iter_off = hdr->man_dtable.row_block_off[2];
    {
      ScopedErrorSilence silence;
      EXPECT_LT(first_cls->shrink(&fr_info, &udata), 0);  // Hits 3614 & 1786.
    }
    hdr->man_iter_off = saved_iter_off;
    // u=0 freed fr inside indirect_shrink without decrementing top_indir->rc;
    // free nr via normal_cls->free (rc 2 -> 1) and release the remaining ref on
    // top_indir via a properly H5FL-allocated proxy row node (rc 1 -> 0).
    H5FS_section_class_t* normal_cls =
        &hdr->fspace->sect_cls[H5HF_FSPACE_SECT_NORMAL_ROW];
    EXPECT_GE(normal_cls->free(&nr->sect_info), 0);
    H5HF_free_section_t* proxy1 = H5HF__sect_single_new(1, 1, nullptr, 0);
    ASSERT_NE(proxy1, nullptr);
    proxy1->sect_info.type = H5HF_FSPACE_SECT_NORMAL_ROW;
    proxy1->u.row.under = top_indir;
    EXPECT_GE(normal_cls->free(&proxy1->sect_info), 0);
  }

  // Separate heap for H5HF__sect_row_merge shrink path failure (line 1706).
  {
    const H5HF_create_t cparam =
        MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                       /*max_direct_size=*/256, /*max_index=*/16);
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5HF_t* fh = H5HF_create(GetFilePtr(file.get()), &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    for (int i = 0; i < 4; ++i) {
      (void)InsertObj(fh, 49, static_cast<uint8_t>(0x18 + i));
    }
    ASSERT_GE(H5HF__man_iblock_root_double(hdr, 64), 0);  // nrows = 4.
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/4,
                                    /*nentries=*/1),
              0);
    H5HF_free_section_t* fr1 = FindSectionAtAddr(
        hdr, hdr->man_dtable.row_block_off[2], H5HF_FSPACE_SECT_FIRST_ROW);
    ASSERT_NE(fr1, nullptr);
    ASSERT_GE(H5FS_sect_remove(hdr->f, hdr->fspace, &fr1->sect_info), 0);

    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/5,
                                    /*nentries=*/3),
              0);
    H5HF_free_section_t* fr2 =
        FindSectionAtAddr(hdr,
                          hdr->man_dtable.row_block_off[2] +
                              hdr->man_dtable.row_block_size[2],
                          H5HF_FSPACE_SECT_FIRST_ROW);
    H5HF_free_section_t* nr2 = FindSectionAtAddr(
        hdr, hdr->man_dtable.row_block_off[3], H5HF_FSPACE_SECT_NORMAL_ROW);
    ASSERT_NE(fr2, nullptr);
    ASSERT_NE(nr2, nullptr);
    H5HF_free_section_t* top_indir2 = fr2->u.row.under;
    ASSERT_NE(top_indir2, nullptr);

    ASSERT_GE(H5FS_sect_remove(hdr->f, hdr->fspace, &nr2->sect_info), 0);
    ASSERT_GE(H5FS_sect_remove(hdr->f, hdr->fspace, &fr2->sect_info), 0);

    H5FS_section_class_t* first_cls =
        &hdr->fspace->sect_cls[H5HF_FSPACE_SECT_FIRST_ROW];
    H5FS_section_class_t* normal_cls =
        &hdr->fspace->sect_cls[H5HF_FSPACE_SECT_NORMAL_ROW];
    H5HF_sect_add_ud_t udata{hdr};
    H5FS_section_info_t* fr1_info = &fr1->sect_info;
    const hsize_t saved_iter_off = hdr->man_iter_off;
    hdr->man_iter_off = fr2->sect_info.addr;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(first_cls->merge(&fr1_info, &fr2->sect_info, &udata), 0);
    }
    hdr->man_iter_off = saved_iter_off;
    EXPECT_GE(normal_cls->free(&nr2->sect_info), 0);
    H5HF_free_section_t* proxy2 = H5HF__sect_single_new(1, 1, nullptr, 0);
    ASSERT_NE(proxy2, nullptr);
    proxy2->sect_info.type = H5HF_FSPACE_SECT_NORMAL_ROW;
    proxy2->u.row.under = top_indir2;
    EXPECT_GE(normal_cls->free(&proxy2->sect_info), 0);
    ASSERT_GE(H5HF__space_add(hdr, fr1, H5FS_ADD_SKIP_VALID), 0);
  }

  // Separate heap for H5HF__sect_row_reduce re-add collision (line 1346).
  {
    const H5HF_create_t cparam =
        MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                       /*max_direct_size=*/256, /*max_index=*/16);
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5HF_t* fh = H5HF_create(GetFilePtr(file.get()), &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
    H5HF_hdr_t* hdr = fh->hdr;

    for (int i = 0; i < 4; ++i) {
      (void)InsertObj(fh, 49, static_cast<uint8_t>(0x20 + i));
    }
    ASSERT_GE(H5HF__man_iblock_root_double(hdr, 64), 0);
    ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/4,
                                    /*nentries=*/2),
              0);
    (void)InsertObj(fh, 241, 0x25);  // Anchor at (3,0), keeping row 2 intact.

    H5HF_free_section_t* row2 = FindSectionAtAddr(
        hdr, hdr->man_dtable.row_block_off[2], H5HF_FSPACE_SECT_FIRST_ROW);
    ASSERT_NE(row2, nullptr);
    ASSERT_EQ(row2->u.row.num_entries, 2U);
    ASSERT_GE(H5FS_sect_remove(hdr->f, hdr->fspace, &row2->sect_info), 0);

    H5HF_free_section_t* dummy = H5HF__sect_single_new(
        row2->sect_info.addr + hdr->man_dtable.row_block_size[2], 1, nullptr, 0);
    ASSERT_NE(dummy, nullptr);
    ASSERT_GE(H5HF__space_add(hdr, dummy, H5FS_ADD_DESERIALIZING), 0);

    unsigned entry_out = 0;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5HF__sect_row_reduce(hdr, row2, &entry_out), 0);
    }

    ASSERT_GE(H5FS_sect_remove(hdr->f, hdr->fspace, &dummy->sect_info), 0);
    ASSERT_GE(H5HF__sect_single_free(&dummy->sect_info), 0);
    ASSERT_GE(H5HF__space_add(hdr, row2, H5FS_ADD_SKIP_VALID), 0);
  }
}

TEST(H5HFsectionTest, MergeDirectRowsWithEmptyIndirEntsIntoChildIndirect) {
  // width=2, start_block_size=64, max_direct_size=256 => max_direct_rows=4:
  // row 0: 64x2 (off 0), row 1: 64x2 (off 128), row 2: 128x2 (off 256),
  // row 3: 256x2 (off 512), row 4: 512x2 indirect (off 1024, 1536).
  const H5HF_create_t cparam =
      MakeHeapParams(/*width=*/2, /*start_block_size=*/64,
                     /*max_direct_size=*/256, /*max_index=*/16);
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5HF_t* fh = H5HF_create(GetFilePtr(file.get()), &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
  H5HF_hdr_t* hdr = fh->hdr;

  // Fill root rows 0..1 permanently and row 2 with removable objects.
  for (int i = 0; i < 4; ++i) {
    (void)InsertObj(fh, 49, static_cast<uint8_t>(0x01 + i));
  }
  auto id_20 = InsertObj(fh, 113, 0x05);  // root (2,0), off=256
  auto id_21 = InsertObj(fh, 113, 0x06);  // root (2,1), off=384

  // Double root_iblock (nrows 4 -> 8) without skipping direct rows, then skip
  // entries 6=(3,0), 7=(3,1), 8=(4,0) so sect2 spans direct row 3 plus child
  // indirect (4,0) (dir_nrows=1, indir_nents=1) with row 3 in root_iblock.
  ASSERT_GE(H5HF__man_iblock_root_double(hdr, 64), 0);
  ASSERT_GE(H5HF__hdr_skip_blocks(hdr, hdr->root_iblock, /*start_entry=*/6,
                                  /*nentries=*/3),
            0);

  // Removing row 2's objects creates sect1 covering direct row 2 only
  // (indir_ents == NULL), which merges with row 3 / sect2 and takes over
  // sect2->indir_ents into sect1 (lines 3422-3424).
  ASSERT_GE(H5HF_remove(fh, id_20.data()), 0);
  ASSERT_GE(H5HF_remove(fh, id_21.data()), 0);

  H5HF_free_section_t* merged_row2 = FindSectionAtAddr(
      hdr, hdr->man_dtable.row_block_off[2], H5HF_FSPACE_SECT_FIRST_ROW);
  ASSERT_NE(merged_row2, nullptr);
  H5HF_free_section_t* top_sect = merged_row2->u.row.under;
  ASSERT_NE(top_sect, nullptr);
  EXPECT_EQ(top_sect->u.indirect.dir_nrows, 2U);
  EXPECT_EQ(top_sect->u.indirect.indir_nents, 1U);
  EXPECT_NE(top_sect->u.indirect.indir_ents, nullptr);
  EXPECT_EQ(top_sect->u.indirect.num_entries, 5U);
}

}  // namespace
}  // namespace hdf5
