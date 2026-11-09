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
// Tests free-space manager metadata statistics retrieval across non-persistent,
// persistent, reopened, and public file-strategy callers
// (third_party/hdf5/src/H5FSstat.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FSstat.c

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with `-DNDEBUG`, which omits debug/stats fields from
// `H5C_cache_entry_t` (`H5AC_info_t`), the first member of `H5FS_t` and
// `H5FS_sinfo_t`. Match `NDEBUG` around the prelude and internal headers so
// struct offsets in C++ match `:hdf5`.
#ifndef NDEBUG
#define H5FSSTAT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5FS_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FSpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5FSSTAT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FSSTAT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

// RAII wrapper for pushing/popping an HDF5 API context and restoring tag state.
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

// RAII wrapper around an open H5FS_t* handle.
class ScopedFspace {
 public:
  ScopedFspace() = default;
  ScopedFspace(H5F_t* f, H5FS_t* fs) : f_(f), fs_(fs) {}
  ~ScopedFspace() { reset(); }

  ScopedFspace(const ScopedFspace&) = delete;
  ScopedFspace& operator=(const ScopedFspace&) = delete;

  void reset(H5F_t* f = nullptr, H5FS_t* next = nullptr) {
    if (fs_ != nullptr && f_ != nullptr) {
      H5FS_close(f_, fs_);
    }
    f_ = f;
    fs_ = next;
  }

  H5FS_t* get() const { return fs_; }

 private:
  H5F_t* f_ = nullptr;
  H5FS_t* fs_ = nullptr;
};

struct StatTestSection {
  H5FS_section_info_t sect_info{};
  uint32_t tag = 0;
};

StatTestSection* AllocSection(unsigned type, haddr_t addr, hsize_t size,
                              H5FS_section_state_t state, uint32_t tag = 0) {
  auto* s = static_cast<StatTestSection*>(std::calloc(1, sizeof(StatTestSection)));
  if (s != nullptr) {
    s->sect_info.addr = addr;
    s->sect_info.size = size;
    s->sect_info.type = type;
    s->sect_info.state = state;
    s->tag = tag;
  }
  return s;
}

herr_t FreeSection(H5FS_section_info_t* sect) {
  std::free(sect);
  return 0;
}

herr_t SerializeSection(const H5FS_section_class_t* /*cls*/,
                        const H5FS_section_info_t* sect, uint8_t* buf) {
  const auto* s = reinterpret_cast<const StatTestSection*>(sect);
  buf[0] = static_cast<uint8_t>(s->tag & 0xffU);
  buf[1] = static_cast<uint8_t>((s->tag >> 8) & 0xffU);
  buf[2] = static_cast<uint8_t>((s->tag >> 16) & 0xffU);
  buf[3] = static_cast<uint8_t>((s->tag >> 24) & 0xffU);
  return 0;
}

H5FS_section_info_t* DeserializeSection(const H5FS_section_class_t* cls,
                                        const uint8_t* buf, haddr_t sect_addr,
                                        hsize_t sect_size,
                                        unsigned* /*des_flags*/) {
  const uint32_t tag = static_cast<uint32_t>(buf[0]) |
                       (static_cast<uint32_t>(buf[1]) << 8) |
                       (static_cast<uint32_t>(buf[2]) << 16) |
                       (static_cast<uint32_t>(buf[3]) << 24);
  StatTestSection* s =
      AllocSection(cls->type, sect_addr, sect_size, H5FS_SECT_SERIALIZED, tag);
  return s != nullptr ? &s->sect_info : nullptr;
}

const H5FS_section_class_t kSerialClass = {
    /*type=*/0,
    /*serial_size=*/4,
    /*flags=*/0,
    /*cls_private=*/nullptr,
    /*init_cls=*/nullptr,
    /*term_cls=*/nullptr,
    /*add=*/nullptr,
    /*serialize=*/SerializeSection,
    /*deserialize=*/DeserializeSection,
    /*can_merge=*/nullptr,
    /*merge=*/nullptr,
    /*can_shrink=*/nullptr,
    /*shrink=*/nullptr,
    /*free=*/FreeSection,
    /*valid=*/nullptr,
    /*split=*/nullptr,
    /*debug=*/nullptr,
};

const H5FS_section_class_t kGhostClass = {
    /*type=*/1,
    /*serial_size=*/0,
    /*flags=*/H5FS_CLS_GHOST_OBJ | H5FS_CLS_SEPAR_OBJ,
    /*cls_private=*/nullptr,
    /*init_cls=*/nullptr,
    /*term_cls=*/nullptr,
    /*add=*/nullptr,
    /*serialize=*/nullptr,
    /*deserialize=*/nullptr,
    /*can_merge=*/nullptr,
    /*merge=*/nullptr,
    /*can_shrink=*/nullptr,
    /*shrink=*/nullptr,
    /*free=*/FreeSection,
    /*valid=*/nullptr,
    /*split=*/nullptr,
    /*debug=*/nullptr,
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5FS_create_t MakeDefaultCreateParams() {
  H5FS_create_t cparam{};
  cparam.client = H5FS_CLIENT_FHEAP_ID;
  cparam.shrink_percent = 20;
  cparam.expand_percent = 120;
  cparam.max_sect_addr = 32;
  cparam.max_sect_size = 65536;
  return cparam;
}

std::vector<uint8_t> SnapshotFileImage(hid_t file_id) {
  const ssize_t size = H5Fget_file_image(file_id, nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::vector<uint8_t> image(static_cast<size_t>(size));
  const ssize_t copied = H5Fget_file_image(file_id, image.data(), image.size());
  if (copied != size) {
    return {};
  }
  return image;
}

TEST(H5FSstatTest, NonPersistentEmptyAndPopulatedStatsReflectAddressSizes) {
  for (const size_t addr_size : {size_t{4}, size_t{8}}) {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_sizes(fcpl.get(), addr_size, addr_size), 0);

    ScopedFile file = CreateInMemoryFile("h5fsstat_nonpersist.h5",
                                         H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(nullptr, f);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    const H5FS_create_t cparam = MakeDefaultCreateParams();
    const H5FS_section_class_t* classes[] = {&kSerialClass, &kGhostClass};
    ScopedFspace fs(
        f, H5FS_create(f, /*fs_addr=*/nullptr, &cparam, /*nclasses=*/2, classes,
                       /*cls_init_udata=*/nullptr, /*alignment=*/0,
                       /*threshold=*/0));
    ASSERT_NE(nullptr, fs.get());

    // Pre-fill stats with sentinels so every field assignment is verified.
    H5FS_stat_t stats{};
    std::memset(&stats, 0xaa, sizeof(stats));
    ASSERT_GE(H5FS_stat_info(f, fs.get(), &stats), 0);
    EXPECT_EQ(0u, stats.tot_space);
    EXPECT_EQ(0u, stats.tot_sect_count);
    EXPECT_EQ(0u, stats.serial_sect_count);
    EXPECT_EQ(0u, stats.ghost_sect_count);
    EXPECT_EQ(HADDR_UNDEF, stats.addr);
    EXPECT_EQ(static_cast<hsize_t>(H5FS_HEADER_SIZE(f)), stats.hdr_size);
    EXPECT_EQ(HADDR_UNDEF, stats.sect_addr);
    EXPECT_EQ(0u, stats.alloc_sect_size);
    EXPECT_EQ(0u, stats.sect_size);

    // Add 2 serializable sections and 1 ghost section.
    auto* s1 = AllocSection(0, 1000, 128, H5FS_SECT_LIVE, 11);
    auto* s2 = AllocSection(0, 2000, 256, H5FS_SECT_LIVE, 22);
    auto* g1 = AllocSection(1, 3000, 64, H5FS_SECT_LIVE, 33);
    ASSERT_NE(nullptr, s1);
    ASSERT_NE(nullptr, s2);
    ASSERT_NE(nullptr, g1);

    ASSERT_GE(H5FS_sect_add(f, fs.get(), &s1->sect_info, 0, nullptr, nullptr),
              0);
    ASSERT_GE(H5FS_sect_add(f, fs.get(), &s2->sect_info, 0, nullptr, nullptr),
              0);
    ASSERT_GE(H5FS_sect_add(f, fs.get(), &g1->sect_info, 0, nullptr, nullptr),
              0);

    std::memset(&stats, 0x55, sizeof(stats));
    ASSERT_GE(H5FS_stat_info(f, fs.get(), &stats), 0);
    EXPECT_EQ(448u, stats.tot_space);
    EXPECT_EQ(3u, stats.tot_sect_count);
    EXPECT_EQ(2u, stats.serial_sect_count);
    EXPECT_EQ(1u, stats.ghost_sect_count);
    EXPECT_EQ(HADDR_UNDEF, stats.addr);
    EXPECT_EQ(static_cast<hsize_t>(H5FS_HEADER_SIZE(f)), stats.hdr_size);
    EXPECT_EQ(HADDR_UNDEF, stats.sect_addr);
    EXPECT_EQ(0u, stats.alloc_sect_size);
    EXPECT_GT(stats.sect_size, 0u);
    EXPECT_EQ(fs.get()->sect_size, stats.sect_size);
  }
}

TEST(H5FSstatTest, PersistentAllocatedAndReopenedStatsRoundTrip) {
  haddr_t fs_addr = HADDR_UNDEF;
  H5FS_stat_t flushed_stats{};
  std::vector<uint8_t> image;

  const H5FS_section_class_t* classes[] = {&kSerialClass, &kGhostClass};
  const H5FS_create_t cparam = MakeDefaultCreateParams();

  // 1. Create persistent free-space manager, populate sections, allocate
  // section info storage, and flush to an in-memory file image.
  {
    ScopedFile file = CreateInMemoryFile("h5fsstat_persist.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(nullptr, f);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    ScopedFspace fs(f, H5FS_create(f, &fs_addr, &cparam, /*nclasses=*/2,
                                   classes, /*cls_init_udata=*/nullptr,
                                   /*alignment=*/0, /*threshold=*/0));
    ASSERT_NE(nullptr, fs.get());
    ASSERT_NE(HADDR_UNDEF, fs_addr);

    auto* s1 = AllocSection(0, 4096, 512, H5FS_SECT_LIVE, 101);
    auto* s2 = AllocSection(0, 8192, 512, H5FS_SECT_LIVE, 202);
    auto* g1 = AllocSection(1, 12288, 256, H5FS_SECT_LIVE, 303);
    ASSERT_NE(nullptr, s1);
    ASSERT_NE(nullptr, s2);
    ASSERT_NE(nullptr, g1);

    ASSERT_GE(H5FS_sect_add(f, fs.get(), &s1->sect_info, 0, nullptr, nullptr),
              0);
    ASSERT_GE(H5FS_sect_add(f, fs.get(), &s2->sect_info, 0, nullptr, nullptr),
              0);
    ASSERT_GE(H5FS_sect_add(f, fs.get(), &g1->sect_info, 0, nullptr, nullptr),
              0);

    H5FS_stat_t before_alloc{};
    ASSERT_GE(H5FS_stat_info(f, fs.get(), &before_alloc), 0);
    EXPECT_EQ(fs_addr, before_alloc.addr);
    EXPECT_EQ(HADDR_UNDEF, before_alloc.sect_addr);
    EXPECT_EQ(1280u, before_alloc.tot_space);
    EXPECT_EQ(3u, before_alloc.tot_sect_count);
    EXPECT_EQ(2u, before_alloc.serial_sect_count);
    EXPECT_EQ(1u, before_alloc.ghost_sect_count);

    ASSERT_GE(H5FS_alloc_sect(f, fs.get()), 0);
    ASSERT_GE(H5FS_stat_info(f, fs.get(), &flushed_stats), 0);
    EXPECT_EQ(fs_addr, flushed_stats.addr);
    EXPECT_NE(HADDR_UNDEF, flushed_stats.sect_addr);
    EXPECT_GT(flushed_stats.alloc_sect_size, 0u);
    EXPECT_EQ(flushed_stats.sect_size, flushed_stats.alloc_sect_size);

    fs.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    image = SnapshotFileImage(file.get());
    ASSERT_FALSE(image.empty());
  }

  // 2. Reopen file image read-only and verify H5FS_open preserves statistics.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(
      H5Fopen("h5fsstat_reopen.h5", H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(reopened.valid());
  H5F_t* rf = GetH5File(reopened.get());
  ASSERT_NE(nullptr, rf);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  ScopedFspace fs(rf, H5FS_open(rf, fs_addr, /*nclasses=*/2, classes,
                                /*cls_init_udata=*/nullptr, /*alignment=*/0,
                                /*threshold=*/0));
  ASSERT_NE(nullptr, fs.get());

  H5FS_stat_t reopened_stats{};
  ASSERT_GE(H5FS_stat_info(rf, fs.get(), &reopened_stats), 0);
  EXPECT_EQ(flushed_stats.addr, reopened_stats.addr);
  EXPECT_EQ(flushed_stats.hdr_size, reopened_stats.hdr_size);
  EXPECT_EQ(flushed_stats.sect_addr, reopened_stats.sect_addr);
  EXPECT_EQ(flushed_stats.alloc_sect_size, reopened_stats.alloc_sect_size);
  EXPECT_EQ(flushed_stats.sect_size, reopened_stats.sect_size);
  EXPECT_EQ(flushed_stats.tot_space, reopened_stats.tot_space);
  EXPECT_EQ(flushed_stats.tot_sect_count, reopened_stats.tot_sect_count);
  EXPECT_EQ(flushed_stats.serial_sect_count, reopened_stats.serial_sect_count);
  EXPECT_EQ(flushed_stats.ghost_sect_count, reopened_stats.ghost_sect_count);
}

TEST(H5FSstatTest, PublicFileFreeSpaceQueriesExerciseFileFreeSpaceStats) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_FSM_AGGR,
                                       /*persist=*/true, /*threshold=*/1),
            0);

  ScopedFile file =
      CreateInMemoryFile("h5fsstat_public.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kDims[1] = {64};
  ScopedSpace space(H5Screate_simple(1, kDims, kDims));
  ASSERT_TRUE(space.valid());

  for (const char* name : {"/d0", "/d1", "/d2"}) {
    ScopedDataset dset(H5Dcreate2(file.get(), name, H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<int32_t> vals(64, 7);
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, vals.data()),
              0);
  }

  ASSERT_GE(H5Ldelete(file.get(), "/d1", H5P_DEFAULT), 0);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  const hssize_t free_bytes = H5Fget_freespace(file.get());
  EXPECT_GT(free_bytes, 0);

  const ssize_t nsects =
      H5Fget_free_sections(file.get(), H5FD_MEM_DEFAULT, 0, nullptr);
  EXPECT_GE(nsects, 1);
}

}  // namespace
}  // namespace hdf5
