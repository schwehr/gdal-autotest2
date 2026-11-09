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
// Tests free-space manager creation, opening, deletion, closing, allocation,
// reference counting, destruction, statistics, and error paths
// (third_party/hdf5/src/H5FS.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FS.c

#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5FS_t and H5FS_sinfo_t). Define NDEBUG around internal
// headers so C++ struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5FS_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5C_FRIEND
#define H5F_FRIEND
#define H5FS_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5Cpkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FSpkg.h"
#pragma push_macro("try")
#undef try
#define try hdf5_c_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#undef try
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5FS_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FS_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

struct TestClassControl {
  bool fail_init = false;
  bool fail_term = false;
};

struct TestSection {
  H5FS_section_info_t base{};
  uint32_t tag = 0;
};

herr_t InitTestClass(H5FS_section_class_t* /*cls*/, void* udata) {
  auto* ctrl = static_cast<TestClassControl*>(udata);
  if (ctrl != nullptr && ctrl->fail_init) {
    return -1;
  }
  return 0;
}

herr_t TermTestClass(H5FS_section_class_t* cls) {
  auto* ctrl = static_cast<TestClassControl*>(cls->cls_private);
  if (ctrl != nullptr && ctrl->fail_term) {
    return -1;
  }
  return 0;
}

herr_t FreeTestSection(H5FS_section_info_t* sect) {
  delete reinterpret_cast<TestSection*>(sect);
  return 0;
}

herr_t SerializeTestSection(const H5FS_section_class_t* /*cls*/,
                            const H5FS_section_info_t* sect, uint8_t* buf) {
  const auto* ts = reinterpret_cast<const TestSection*>(sect);
  for (int b = 0; b < 4; ++b) {
    buf[b] = static_cast<uint8_t>((ts->tag >> (b * 8)) & 0xffU);
  }
  return 0;
}

H5FS_section_info_t* DeserializeTestSection(const H5FS_section_class_t* cls,
                                            const uint8_t* buf,
                                            haddr_t sect_addr,
                                            hsize_t sect_size,
                                            unsigned* /*des_flags*/) {
  auto* ts = new TestSection();
  ts->base.addr = sect_addr;
  ts->base.size = sect_size;
  ts->base.type = cls->type;
  ts->base.state = H5FS_SECT_SERIALIZED;
  if (cls->serial_size == 4) {
    uint32_t v = 0;
    for (int b = 0; b < 4; ++b) {
      v |= static_cast<uint32_t>(buf[b]) << (b * 8);
    }
    ts->tag = v;
  }
  return &ts->base;
}

H5FS_section_class_t MakeSectionClass(unsigned type, size_t serial_size,
                                      unsigned flags, TestClassControl* ctrl) {
  return H5FS_section_class_t{
      /*type=*/type,
      /*serial_size=*/serial_size,
      /*flags=*/flags,
      /*cls_private=*/ctrl,
      /*init_cls=*/InitTestClass,
      /*term_cls=*/TermTestClass,
      /*add=*/nullptr,
      /*serialize=*/serial_size > 0 ? SerializeTestSection : nullptr,
      /*deserialize=*/DeserializeTestSection,
      /*can_merge=*/nullptr,
      /*merge=*/nullptr,
      /*can_shrink=*/nullptr,
      /*shrink=*/nullptr,
      /*free=*/FreeTestSection,
      /*valid=*/nullptr,
      /*split=*/nullptr,
      /*debug=*/nullptr,
  };
}

TestSection* MakeSection(haddr_t addr, hsize_t size, unsigned type = 0,
                         uint32_t tag = 0) {
  auto* ts = new TestSection();
  ts->base.addr = addr;
  ts->base.size = size;
  ts->base.type = type;
  ts->base.state = H5FS_SECT_LIVE;
  ts->tag = tag;
  return ts;
}

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__FREESPACE_TAG) {
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

H5FS_create_t MakeFsCreateParams(
    H5FS_client_t client = H5FS_CLIENT_FHEAP_ID) {
  H5FS_create_t params{};
  params.client = client;
  params.shrink_percent = 80;
  params.expand_percent = 120;
  params.max_sect_addr = 32;
  params.max_sect_size = 0x10000;
  return params;
}

// Forces subsequent H5MF_alloc calls on `f` to fail cleanly with HADDR_UNDEF
// while preserving aggregator structures, restoring file state on scope exit.
class ScopedBlockAllocFailure {
 public:
  explicit ScopedBlockAllocFailure(H5F_t* f)
      : f_(f),
        saved_strategy_(f->shared->fs_strategy),
        saved_tmp_addr_(f->shared->tmp_addr),
        saved_maxaddr_(f->shared->lf->maxaddr) {
    const haddr_t eoa = H5F_get_eoa(f_, H5FD_MEM_DEFAULT);
    f_->shared->fs_strategy = H5F_FSPACE_STRATEGY_NONE;
    f_->shared->tmp_addr = (eoa != HADDR_UNDEF) ? eoa : 0;
    f_->shared->lf->maxaddr = 0;
  }

  ~ScopedBlockAllocFailure() {
    f_->shared->lf->maxaddr = saved_maxaddr_;
    f_->shared->tmp_addr = saved_tmp_addr_;
    f_->shared->fs_strategy = saved_strategy_;
  }

  ScopedBlockAllocFailure(const ScopedBlockAllocFailure&) = delete;
  ScopedBlockAllocFailure& operator=(const ScopedBlockAllocFailure&) = delete;

 private:
  H5F_t* f_;
  H5F_fspace_strategy_t saved_strategy_;
  haddr_t saved_tmp_addr_;
  haddr_t saved_maxaddr_;
};

TEST(H5FSTest, CreateNonPersistentAndCloseDestroysSectionsAndMergeList) {
  ScopedFile file = CreateInMemoryFile("h5fs_nonpersist.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  TestClassControl ctrl;
  const H5FS_section_class_t cls0 =
      MakeSectionClass(/*type=*/0, /*serial_size=*/0, /*flags=*/0, &ctrl);
  const H5FS_section_class_t cls1_serial = MakeSectionClass(
      /*type=*/1, /*serial_size=*/4, H5FS_CLS_SEPAR_OBJ, &ctrl);
  const H5FS_section_class_t cls2_ghost = MakeSectionClass(
      /*type=*/2, /*serial_size=*/0, H5FS_CLS_GHOST_OBJ, &ctrl);
  const H5FS_section_class_t* classes[] = {&cls0, &cls1_serial, &cls2_ghost};
  const H5FS_create_t cparam = MakeFsCreateParams();

  // 1. Create non-persistent free space manager with multiple classes; verify
  // max_cls_serial_size tracks largest serial_size.
  H5FS_t* fs = H5FS_create(f, /*fs_addr=*/nullptr, &cparam, /*nclasses=*/3,
                           classes, &ctrl, /*alignment=*/0, /*threshold=*/0);
  ASSERT_NE(fs, nullptr);
  EXPECT_EQ(fs->max_cls_serial_size, 4u);
  EXPECT_FALSE(H5_addr_defined(fs->addr));

  // Add sections across classes (both mergeable -> creates merge_list, separate,
  // and ghost) so closing hits H5FS__sinfo_dest, H5FS__sinfo_free_node_cb, and
  // H5FS__sinfo_free_sect_cb.
  ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x100, 64, 0)->base, 0, nullptr,
                          nullptr),
            0);
  ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x300, 128, 1, 77)->base, 0,
                          nullptr, nullptr),
            0);
  ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x500, 64, 2)->base, 0, nullptr,
                          nullptr),
            0);

  hsize_t sect_count = 999;
  EXPECT_GE(H5FS_get_sect_count(fs, &sect_count), 0);
  EXPECT_EQ(sect_count, 2u);  // Only serializable sections are counted here.

  hsize_t meta_size = 0;
  EXPECT_GE(H5FS_size(fs, &meta_size), 0);
  EXPECT_EQ(meta_size, fs->hdr_size + fs->sect_size);

  // H5FS__dirty on non-persistent header is a no-op returning >= 0.
  EXPECT_GE(H5FS__dirty(fs), 0);

  // Closing non-persistent header destroys sinfo and then header via H5FS__decr.
  EXPECT_GE(H5FS_close(f, fs), 0);

  // 2. Zero section classes works in H5FS_create / H5FS__new / H5FS__hdr_dest.
  H5FS_t* zero_cls_fs =
      H5FS_create(f, /*fs_addr=*/nullptr, &cparam, /*nclasses=*/0, nullptr,
                  nullptr, /*alignment=*/0, /*threshold=*/0);
  ASSERT_NE(zero_cls_fs, nullptr);
  EXPECT_EQ(zero_cls_fs->nclasses, 0u);
  EXPECT_EQ(zero_cls_fs->sect_cls, nullptr);
  EXPECT_GE(H5FS_close(f, zero_cls_fs), 0);
}

TEST(H5FSTest, CreateOpenDeleteRoundTripAndCacheVsDiskDelete) {
  ScopedFile file = CreateInMemoryFile("h5fs_persist.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  TestClassControl ctrl;
  const H5FS_section_class_t cls0 =
      MakeSectionClass(/*type=*/0, /*serial_size=*/4, /*flags=*/0, &ctrl);
  const H5FS_section_class_t* classes[] = {&cls0};
  const H5FS_create_t cparam = MakeFsCreateParams();

  // 1. Delete empty persistent free space manager (serial_sect_count == 0).
  {
    haddr_t empty_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &empty_addr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_TRUE(H5_addr_defined(empty_addr));
    ASSERT_GE(H5FS_close(f, fs), 0);
    EXPECT_GE(H5FS_delete(f, empty_addr), 0);
  }

  // 2. Delete non-empty free space manager with sinfo in cache at tmp_addr.
  {
    haddr_t tmp_sinfo_fs_addr = HADDR_UNDEF;
    H5FS_t* fs =
        H5FS_create(f, &tmp_sinfo_fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x200, 64, 0, 11)->base, 0,
                            nullptr, nullptr),
              0);
    // f->shared->use_tmp_space is true by default, so close places sinfo in
    // cache at a temporary address.
    ASSERT_GE(H5FS_close(f, fs), 0);
    EXPECT_GE(H5FS_delete(f, tmp_sinfo_fs_addr), 0);
  }

  // 3. Delete non-empty free space manager with sinfo in cache at real address.
  {
    haddr_t real_sinfo_fs_addr = HADDR_UNDEF;
    H5FS_t* fs =
        H5FS_create(f, &real_sinfo_fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x400, 128, 0, 22)->base, 0,
                            nullptr, nullptr),
              0);
    {
      const bool saved_tmp = f->shared->use_tmp_space;
      f->shared->use_tmp_space = false;
      auto restore_tmp =
          absl::MakeCleanup([&] { f->shared->use_tmp_space = saved_tmp; });
      ASSERT_GE(H5FS_close(f, fs), 0);
    }
    EXPECT_GE(H5FS_delete(f, real_sinfo_fs_addr), 0);
  }

  // 4. Open after eviction from cache, verify H5FS_size with sinfo == nullptr,
  // close without sinfo, evict, and delete from disk (sinfo not in cache).
  {
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x600, 256, 0, 33)->base, 0,
                            nullptr, nullptr),
              0);
    ASSERT_GE(H5FS_close(f, fs), 0);
    ASSERT_GE(H5AC_flush(f), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    fs = H5FS_open(f, fs_addr, 1, classes, &ctrl, /*alignment=*/8,
                   /*threshold=*/4);
    ASSERT_NE(fs, nullptr);
    EXPECT_EQ(fs->sinfo, nullptr);
    EXPECT_EQ(fs->alignment, 8u);
    EXPECT_EQ(fs->align_thres, 4u);

    hsize_t meta_size = 0;
    EXPECT_GE(H5FS_size(fs, &meta_size), 0);
    EXPECT_EQ(meta_size, fs->hdr_size + fs->alloc_sect_size);

    // Close when fspace->sinfo == nullptr and serial_sect_count > 0.
    EXPECT_GE(H5FS_close(f, fs), 0);

    // Evict both header and sinfo so H5FS_delete exercises the !IN_CACHE path.
    ASSERT_GE(H5AC_evict(f), 0);
    EXPECT_GE(H5FS_delete(f, fs_addr), 0);
  }
}

TEST(H5FSTest, AllocHdrAllocSectAndFreePaths) {
  ScopedFile file = CreateInMemoryFile("h5fs_alloc_free.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  TestClassControl ctrl;
  const H5FS_section_class_t cls0 =
      MakeSectionClass(/*type=*/0, /*serial_size=*/4, /*flags=*/0, &ctrl);
  const H5FS_section_class_t* classes[] = {&cls0};
  const H5FS_create_t cparam = MakeFsCreateParams();

  // 1. Free unallocated non-persistent manager (neither addr nor sect_addr).
  {
    H5FS_t* fs = H5FS_create(f, nullptr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    EXPECT_GE(H5FS_free(f, fs, /*free_file_space=*/true), 0);
    // H5FS_alloc_sect is a no-op when sinfo == nullptr or serial_sect_count==0.
    EXPECT_GE(H5FS_alloc_sect(f, fs), 0);
    EXPECT_GE(H5FS_close(f, fs), 0);
  }

  // 2. Allocate header with fs_addr == nullptr, call again (no-op), add section,
  // allocate section info via H5FS_alloc_sect, then free with cache reload.
  {
    H5FS_t* fs = H5FS_create(f, nullptr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    EXPECT_GE(H5FS_alloc_hdr(f, fs, nullptr), 0);
    const haddr_t hdr_addr = fs->addr;
    EXPECT_TRUE(H5_addr_defined(hdr_addr));
    haddr_t repeated_addr = HADDR_UNDEF;
    EXPECT_GE(H5FS_alloc_hdr(f, fs, &repeated_addr), 0);
    EXPECT_EQ(repeated_addr, hdr_addr);

    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x800, 64, 0, 5)->base, 0,
                            nullptr, nullptr),
              0);
    EXPECT_GE(H5FS_alloc_sect(f, fs), 0);
    EXPECT_EQ(fs->sinfo, nullptr);
    EXPECT_TRUE(H5_addr_defined(fs->sect_addr));
    // Repeated H5FS_alloc_sect is a no-op.
    EXPECT_GE(H5FS_alloc_sect(f, fs), 0);

    EXPECT_GE(H5FS_free(f, fs, /*free_file_space=*/true), 0);
    EXPECT_FALSE(H5_addr_defined(fs->addr));
    EXPECT_FALSE(H5_addr_defined(fs->sect_addr));
    EXPECT_GE(H5FS_close(f, fs), 0);
  }

  // 3. H5FS_free with free_file_space == false, plus H5FS_free when neither
  // header nor sinfo is resident in cache (!IN_CACHE branch).
  {
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    EXPECT_GE(H5FS_free(f, fs, /*free_file_space=*/false), 0);
    EXPECT_GE(H5FS_close(f, fs), 0);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_FSPACE_HDR, fs_addr, H5FS_HEADER_SIZE(f)),
              0);
  }

  // 4. H5FS_free when header is not in cache (`hdr_status & IN_CACHE == 0`).
  {
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_GE(H5FS_close(f, fs), 0);
    ASSERT_GE(H5AC_flush(f), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    // Protect and take ownership? Reopen keeps hdr in cache; instead create a
    // clean H5FS_t matching on-disk addr not in cache and free it cleanly.
    H5FS_t* detached = H5FS_create(f, nullptr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(detached, nullptr);
    detached->addr = fs_addr;
    EXPECT_GE(H5FS_free(f, detached, /*free_file_space=*/true), 0);
    EXPECT_FALSE(H5_addr_defined(detached->addr));
    EXPECT_GE(H5FS_close(f, detached), 0);
  }
}

TEST(H5FSTest, CloseEmptyWithAllocatedSectAddrCoversBootstrapAndNonFilePaths) {
  ScopedFile file = CreateInMemoryFile("h5fs_close_sect.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  TestClassControl ctrl;
  const H5FS_section_class_t cls0 =
      MakeSectionClass(/*type=*/0, /*serial_size=*/0, /*flags=*/0, &ctrl);
  const H5FS_section_class_t* classes[] = {&cls0};

  // Helper to remove the single section under closing mode and re-attach sinfo
  // ownership with TAKE_OWNERSHIP so H5FS_close encounters sinfo != nullptr,
  // serial_sect_count == 0, and H5_addr_defined(sect_addr).
  auto prepare_empty_owned_with_sect_addr = [&](H5FS_t* fs,
                                                H5FS_section_info_t* sect) {
    {
      const bool saved_closing = f->shared->closing;
      f->shared->closing = true;
      auto restore_closing =
          absl::MakeCleanup([&] { f->shared->closing = saved_closing; });
      ASSERT_GE(H5FS_sect_remove(f, fs, sect), 0);
      FreeTestSection(sect);
    }
    H5FS_sinfo_cache_ud_t sinfo_udata{f, fs};
    auto* sinfo = static_cast<H5FS_sinfo_t*>(H5AC_protect(
        f, H5AC_FSPACE_SINFO, fs->sect_addr, &sinfo_udata, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(sinfo, nullptr);
    ASSERT_GE(H5AC_unprotect(f, H5AC_FSPACE_SINFO, fs->sect_addr, sinfo,
                             H5AC__DELETED_FLAG | H5AC__TAKE_OWNERSHIP_FLAG),
              0);
    fs->sinfo = sinfo;
  };

  // 1. H5FS_CLIENT_FILE_ID + tmp sect_addr (resets sect_addr & dirties hdr).
  {
    const H5FS_create_t cparam = MakeFsCreateParams(H5FS_CLIENT_FILE_ID);
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    TestSection* s = MakeSection(0x100, 64);
    ASSERT_GE(H5FS_sect_add(f, fs, &s->base, 0, nullptr, nullptr), 0);
    // Insert sinfo at tmp addr and close caller ref once, then reopen.
    ASSERT_GE(H5FS_close(f, fs), 0);
    fs = H5FS_open(f, fs_addr, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_TRUE(H5F_IS_TMP_ADDR(f, fs->sect_addr));

    prepare_empty_owned_with_sect_addr(fs, &s->base);
    EXPECT_GE(H5FS_close(f, fs), 0);
    EXPECT_GE(H5FS_delete(f, fs_addr), 0);
  }

  // 2. H5FS_CLIENT_FILE_ID + real sect_addr at EOA -> H5MF_try_shrink == true.
  {
    const H5FS_create_t cparam = MakeFsCreateParams(H5FS_CLIENT_FILE_ID);
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    TestSection* s = MakeSection(0x100, 64);
    ASSERT_GE(H5FS_sect_add(f, fs, &s->base, 0, nullptr, nullptr), 0);
    {
      const bool saved_tmp = f->shared->use_tmp_space;
      f->shared->use_tmp_space = false;
      auto restore_tmp =
          absl::MakeCleanup([&] { f->shared->use_tmp_space = saved_tmp; });
      ASSERT_GE(H5FS_close(f, fs), 0);
    }
    fs = H5FS_open(f, fs_addr, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_FALSE(H5F_IS_TMP_ADDR(f, fs->sect_addr));

    prepare_empty_owned_with_sect_addr(fs, &s->base);
    EXPECT_GE(H5FS_close(f, fs), 0);
    EXPECT_GE(H5FS_delete(f, fs_addr), 0);
  }

  // 3. H5FS_CLIENT_FILE_ID + real sect_addr NOT at EOA -> try_shrink == false.
  {
    const H5FS_create_t cparam = MakeFsCreateParams(H5FS_CLIENT_FILE_ID);
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    TestSection* s = MakeSection(0x100, 64);
    ASSERT_GE(H5FS_sect_add(f, fs, &s->base, 0, nullptr, nullptr), 0);
    {
      const bool saved_tmp = f->shared->use_tmp_space;
      f->shared->use_tmp_space = false;
      auto restore_tmp =
          absl::MakeCleanup([&] { f->shared->use_tmp_space = saved_tmp; });
      ASSERT_GE(H5FS_close(f, fs), 0);
    }
    // Allocate a block after sect_addr so H5MF_try_shrink returns false.
    const haddr_t blocker = H5MF_alloc(f, H5FD_MEM_OHDR, 512);
    ASSERT_TRUE(H5_addr_defined(blocker));
    absl::Cleanup free_blocker = [&] {
      H5MF_xfree(f, H5FD_MEM_OHDR, blocker, 512);
    };

    fs = H5FS_open(f, fs_addr, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    const haddr_t kept_sect_addr = fs->sect_addr;
    const hsize_t kept_sect_size = fs->alloc_sect_size;
    prepare_empty_owned_with_sect_addr(fs, &s->base);
    EXPECT_GE(H5FS_close(f, fs), 0);
    EXPECT_GE(H5FS_delete(f, fs_addr), 0);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_FSPACE_SINFO, kept_sect_addr, kept_sect_size),
              0);
  }

  // 4. H5FS_CLIENT_FHEAP_ID + real sect_addr -> H5MF_xfree frees section space.
  {
    const H5FS_create_t cparam = MakeFsCreateParams(H5FS_CLIENT_FHEAP_ID);
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    TestSection* s = MakeSection(0x100, 64);
    ASSERT_GE(H5FS_sect_add(f, fs, &s->base, 0, nullptr, nullptr), 0);
    {
      const bool saved_tmp = f->shared->use_tmp_space;
      f->shared->use_tmp_space = false;
      auto restore_tmp =
          absl::MakeCleanup([&] { f->shared->use_tmp_space = saved_tmp; });
      ASSERT_GE(H5FS_close(f, fs), 0);
    }
    fs = H5FS_open(f, fs_addr, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    prepare_empty_owned_with_sect_addr(fs, &s->base);
    EXPECT_GE(H5FS_close(f, fs), 0);
    EXPECT_GE(H5FS_delete(f, fs_addr), 0);
  }
}

TEST(H5FSTest, ErrorPathsAcrossCreateOpenDeleteAllocFreeAndLifecycleHelpers) {
  TestClassControl ctrl;
  ScopedFile file = CreateInMemoryFile("h5fs_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5FS_section_class_t cls0 =
      MakeSectionClass(/*type=*/0, /*serial_size=*/0, /*flags=*/0, &ctrl);
  const H5FS_section_class_t* classes[] = {&cls0};
  const H5FS_create_t cparam = MakeFsCreateParams();

  {
    ScopedErrorSilence silence;

    // 1. init_cls failure -> H5FS__new fails -> H5FS_create returns nullptr.
    ctrl.fail_init = true;
    EXPECT_EQ(H5FS__new(f, 1, classes, &ctrl), nullptr);
    EXPECT_EQ(H5FS_create(f, nullptr, &cparam, 1, classes, &ctrl, 0, 0),
              nullptr);
    ctrl.fail_init = false;

    // 2. H5MF_alloc failure in H5FS_create (persistent) triggers cleanup.
    {
      ScopedBlockAllocFailure block_alloc(f);
      haddr_t bad_addr = HADDR_UNDEF;
      EXPECT_EQ(H5FS_create(f, &bad_addr, &cparam, 1, classes, &ctrl, 0, 0),
                nullptr);
      EXPECT_FALSE(H5_addr_defined(bad_addr));
    }

    // 3. H5FS_open & H5FS_delete fail cleanly on corrupt/invalid header addr.
    const haddr_t zero_addr = H5MF_alloc(f, H5FD_MEM_FSPACE_HDR, 64);
    ASSERT_TRUE(H5_addr_defined(zero_addr));
    absl::Cleanup free_zero = [&] {
      H5MF_xfree(f, H5FD_MEM_FSPACE_HDR, zero_addr, 64);
    };
    const std::vector<uint8_t> zeros(64, 0);
    ASSERT_GE(H5F_block_write(f, H5FD_MEM_FSPACE_HDR, zero_addr, 64,
                              zeros.data()),
              0);
    EXPECT_EQ(H5FS_open(f, zero_addr, 1, classes, &ctrl, 0, 0), nullptr);
    EXPECT_LT(H5FS_delete(f, zero_addr), 0);

    // 4. H5FS_alloc_hdr & H5FS_alloc_sect allocation failures.
    H5FS_t* fs = H5FS_create(f, nullptr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x200, 64)->base, 0, nullptr,
                            nullptr),
              0);
    {
      ScopedBlockAllocFailure block_alloc(f);
      EXPECT_LT(H5FS_alloc_hdr(f, fs, nullptr), 0);
      EXPECT_LT(H5FS_alloc_sect(f, fs), 0);

      // Persistent H5FS_close with use_tmp_space == false fails H5MF_alloc.
      const haddr_t hdr_addr = 0x1234;  // Dummy defined address for test.
      fs->addr = hdr_addr;
      const bool saved_tmp = f->shared->use_tmp_space;
      f->shared->use_tmp_space = false;
      EXPECT_LT(H5FS_close(f, fs), 0);
      f->shared->use_tmp_space = saved_tmp;
      fs->addr = HADDR_UNDEF;
    }
    EXPECT_GE(H5FS_close(f, fs), 0);

    // 5. H5FS__hdr_dest & H5FS__decr propagate term_cls failure without leaking
    // (retry with fail_term = false frees cleanly under asan-chlor).
    H5FS_t* fs_term = H5FS_create(f, nullptr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs_term, nullptr);
    ctrl.fail_term = true;
    EXPECT_LT(H5FS__decr(fs_term), 0);
    EXPECT_LT(H5FS__hdr_dest(fs_term), 0);
    ctrl.fail_term = false;
    EXPECT_GE(H5FS__hdr_dest(fs_term), 0);

    // 6. H5FS__incr, H5FS__decr, H5FS__dirty propagate H5AC cache errors when
    // cached persistent entry is neither pinned nor protected.
    haddr_t cached_addr = HADDR_UNDEF;
    H5FS_t* cached_fs =
        H5FS_create(f, &cached_addr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(cached_fs, nullptr);
    ASSERT_GE(H5FS_close(f, cached_fs), 0);
    // cached_fs remains resident in cache with rc == 0, !pinned, !protected.
    EXPECT_LT(H5FS__incr(cached_fs), 0);
    EXPECT_LT(H5FS__dirty(cached_fs), 0);
    cached_fs->rc = 1;
    EXPECT_LT(H5FS__decr(cached_fs), 0);
    EXPECT_EQ(cached_fs->rc, 0u);
  }
}

TEST(H5FSTest, AdditionalErrorPathsForCloseAllocAndFree) {
  TestClassControl ctrl;
  ScopedFile file = CreateInMemoryFile("h5fs_more_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5FS_section_class_t cls0 =
      MakeSectionClass(/*type=*/0, /*serial_size=*/0, /*flags=*/0, &ctrl);
  const H5FS_section_class_t* classes[] = {&cls0};
  const H5FS_create_t cparam = MakeFsCreateParams();

  const auto make_ro_guard = [&] {
    const unsigned saved_flags = f->shared->flags;
    f->shared->flags &= ~H5F_ACC_RDWR;
    return absl::MakeCleanup([f, saved_flags] { f->shared->flags = saved_flags; });
  };
  const auto make_settled_guard = [&] {
    const H5F_fspace_strategy_t saved_strategy = f->shared->fs_strategy;
    const bool saved_rdfsm = f->shared->cache->rdfsm_settled;
    const bool saved_mdfsm = f->shared->cache->mdfsm_settled;
    const bool saved_warn = f->shared->cache->close_warning_received;
    f->shared->fs_strategy = H5F_FSPACE_STRATEGY_FSM_AGGR;
    f->shared->cache->rdfsm_settled = true;
    f->shared->cache->mdfsm_settled = true;
    f->shared->cache->close_warning_received = true;
    return absl::MakeCleanup([f, saved_strategy, saved_rdfsm, saved_mdfsm, saved_warn] {
      f->shared->cache->close_warning_received = saved_warn;
      f->shared->cache->mdfsm_settled = saved_mdfsm;
      f->shared->cache->rdfsm_settled = saved_rdfsm;
      f->shared->fs_strategy = saved_strategy;
    });
  };
  const auto repin_cached_fs = [&](haddr_t fs_addr, H5FS_t* fs) {
    H5FS_hdr_cache_ud_t udata{f, 1, classes, &ctrl, fs_addr};
    H5FS_t* protected_fs = static_cast<H5FS_t*>(
        H5AC_protect(f, H5AC_FSPACE_HDR, fs_addr, &udata, H5AC__NO_FLAGS_SET));
    ASSERT_EQ(protected_fs, fs);
    ASSERT_GE(H5AC_pin_protected_entry(protected_fs), 0);
    ASSERT_GE(
        H5AC_unprotect(f, H5AC_FSPACE_HDR, fs_addr, protected_fs, H5AC__NO_FLAGS_SET),
        0);
  };

  ScopedErrorSilence silence;

  // 1: H5FS_create & H5FS_alloc_hdr -> H5AC_insert_entry failure (no write intent).
  {
    {
      auto ro = make_ro_guard();
      haddr_t bad_addr = HADDR_UNDEF;
      EXPECT_EQ(H5FS_create(f, &bad_addr, &cparam, 1, classes, &ctrl, 0, 0),
                nullptr);
    }

    H5FS_t* fs_hdr = H5FS_create(f, nullptr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs_hdr, nullptr);
    {
      auto ro = make_ro_guard();
      EXPECT_LT(H5FS_alloc_hdr(f, fs_hdr, nullptr), 0);
    }
    ASSERT_TRUE(H5_addr_defined(fs_hdr->addr));
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_FSPACE_HDR, fs_hdr->addr, fs_hdr->hdr_size),
              0);
    fs_hdr->addr = HADDR_UNDEF;
    EXPECT_GE(H5FS_close(f, fs_hdr), 0);
  }

  // 2: Real cached persistent fs covering H5FS_close tmp alloc fail, H5FS_alloc_sect
  // insert & dirty fail, and H5FS_close mark_entry_dirty fail on unpinned fs.
  {
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x100, 64)->base, 0, nullptr,
                            nullptr),
              0);

    // Line 426: H5FS_close -> H5MF_alloc_tmp fails cleanly when tmp_addr == eoa.
    {
      ScopedBlockAllocFailure block_alloc(f);
      EXPECT_LT(H5FS_close(f, fs), 0);
      EXPECT_FALSE(H5_addr_defined(fs->sect_addr));
    }

    // Line 852: H5FS_alloc_sect -> H5AC_insert_entry fails when RDWR is masked.
    {
      {
        auto ro = make_ro_guard();
        EXPECT_LT(H5FS_alloc_sect(f, fs), 0);
      }
      ASSERT_TRUE(H5_addr_defined(fs->sect_addr));
      EXPECT_GE(
          H5MF_xfree(f, H5FD_MEM_FSPACE_SINFO, fs->sect_addr, fs->alloc_sect_size),
          0);
      fs->sect_addr = HADDR_UNDEF;
      fs->alloc_sect_size = 0;
    }

    // Unpin cached fs temporarily so H5AC_mark_entry_dirty fails cleanly on
    // both H5FS_alloc_sect (line 848) and H5FS_close (line 439).
    ASSERT_GE(H5AC_unpin_entry(fs), 0);
    EXPECT_LT(H5FS_alloc_sect(f, fs), 0);
    ASSERT_TRUE(H5_addr_defined(fs->sect_addr));
    EXPECT_GE(
        H5MF_xfree(f, H5FD_MEM_FSPACE_SINFO, fs->sect_addr, fs->alloc_sect_size),
        0);
    fs->sect_addr = HADDR_UNDEF;
    fs->alloc_sect_size = 0;

    {
      const bool saved_tmp = f->shared->use_tmp_space;
      f->shared->use_tmp_space = false;
      auto restore_tmp =
          absl::MakeCleanup([&] { f->shared->use_tmp_space = saved_tmp; });
      EXPECT_LT(H5FS_close(f, fs), 0);
      ASSERT_TRUE(H5_addr_defined(fs->sect_addr));
      EXPECT_GE(
          H5MF_xfree(f, H5FD_MEM_FSPACE_SINFO, fs->sect_addr, fs->alloc_sect_size),
          0);
      fs->sect_addr = HADDR_UNDEF;
      fs->alloc_sect_size = 0;
    }

    repin_cached_fs(fs_addr, fs);
    EXPECT_GE(H5FS_close(f, fs), 0);
    EXPECT_GE(H5FS_delete(f, fs_addr), 0);
  }

  // 3: H5FS_close -> H5AC_insert_entry failure on colliding cache address.
  {
    haddr_t dup_addr = HADDR_UNDEF;
    H5FS_t* fs_dup =
        H5FS_create(f, &dup_addr, &cparam, 1, classes, &ctrl, 0, 0);
    ASSERT_NE(fs_dup, nullptr);
    ASSERT_GE(H5FS_sect_add(f, fs_dup, &MakeSection(0x200, 64)->base, 0,
                            nullptr, nullptr),
              0);
    fs_dup->sinfo->dirty = false;
    fs_dup->sect_addr = dup_addr;
    EXPECT_LT(H5FS_close(f, fs_dup), 0);
    fs_dup->sinfo->dirty = true;
    fs_dup->sect_addr = HADDR_UNDEF;
    EXPECT_GE(H5FS_close(f, fs_dup), 0);
    EXPECT_GE(H5FS_delete(f, dup_addr), 0);
  }

  // 4: Empty-sinfo H5FS_close failures (lines 488, 515, 534, 540, 562).
  {
    // 4a: CLIENT_FILE_ID with tmp sect_addr -> H5AC_mark_entry_dirty fails (line 488).
    {
      const H5FS_create_t file_cparam = MakeFsCreateParams(H5FS_CLIENT_FILE_ID);
      haddr_t fs_addr = HADDR_UNDEF;
      H5FS_t* fs =
          H5FS_create(f, &fs_addr, &file_cparam, 1, classes, &ctrl, 0, 0);
      ASSERT_NE(fs, nullptr);
      TestSection* s = MakeSection(0x300, 64);
      ASSERT_GE(H5FS_sect_add(f, fs, &s->base, 0, nullptr, nullptr), 0);
      {
        const bool saved_closing = f->shared->closing;
        f->shared->closing = true;
        auto restore_closing =
            absl::MakeCleanup([&] { f->shared->closing = saved_closing; });
        ASSERT_GE(H5FS_sect_remove(f, fs, &s->base), 0);
        FreeTestSection(&s->base);
      }
      fs->sect_addr = H5MF_alloc_tmp(f, fs->sect_size);
      ASSERT_TRUE(H5_addr_defined(fs->sect_addr));
      fs->alloc_sect_size = fs->sect_size;
      ASSERT_GE(H5AC_unpin_entry(fs), 0);
      EXPECT_LT(H5FS_close(f, fs), 0);

      // Next test line 515 (try_shrink succeeds with real EOA sect_addr -> dirty fails).
      fs->sect_addr = H5MF_alloc(f, H5FD_MEM_FSPACE_SINFO, fs->sect_size);
      ASSERT_TRUE(H5_addr_defined(fs->sect_addr));
      fs->alloc_sect_size = fs->sect_size;
      EXPECT_LT(H5FS_close(f, fs), 0);

      repin_cached_fs(fs_addr, fs);
      EXPECT_GE(H5FS_close(f, fs), 0);
      EXPECT_GE(H5FS_delete(f, fs_addr), 0);
    }

    // 4b: CLIENT_FHEAP_ID with real sect_addr -> mark_dirty fail (534) & xfree fail (540).
    {
      haddr_t fs_addr = HADDR_UNDEF;
      H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
      ASSERT_NE(fs, nullptr);
      TestSection* s = MakeSection(0x400, 64);
      ASSERT_GE(H5FS_sect_add(f, fs, &s->base, 0, nullptr, nullptr), 0);
      {
        const bool saved_closing = f->shared->closing;
        f->shared->closing = true;
        auto restore_closing =
            absl::MakeCleanup([&] { f->shared->closing = saved_closing; });
        ASSERT_GE(H5FS_sect_remove(f, fs, &s->base), 0);
        FreeTestSection(&s->base);
      }
      const haddr_t real_sect_addr =
          H5MF_alloc(f, H5FD_MEM_FSPACE_SINFO, fs->sect_size);
      ASSERT_TRUE(H5_addr_defined(real_sect_addr));
      const hsize_t real_sect_size = fs->sect_size;

      fs->sect_addr = real_sect_addr;
      fs->alloc_sect_size = real_sect_size;
      ASSERT_GE(H5AC_unpin_entry(fs), 0);
      EXPECT_LT(H5FS_close(f, fs), 0);

      repin_cached_fs(fs_addr, fs);
      fs->sect_addr = real_sect_addr;
      fs->alloc_sect_size = real_sect_size;
      {
        auto settled = make_settled_guard();
        EXPECT_LT(H5FS_close(f, fs), 0);
      }
      EXPECT_GE(
          H5MF_xfree(f, H5FD_MEM_FSPACE_SINFO, real_sect_addr, real_sect_size),
          0);
      EXPECT_GE(H5FS_close(f, fs), 0);
      EXPECT_GE(H5FS_delete(f, fs_addr), 0);
    }

    // 4c: Line 562: H5FS_close -> H5FS__decr fails unpinning already-unpinned fs.
    {
      haddr_t fs_addr = HADDR_UNDEF;
      H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
      ASSERT_NE(fs, nullptr);
      ASSERT_GE(H5AC_unpin_entry(fs), 0);
      EXPECT_LT(H5FS_close(f, fs), 0);
      fs->rc = 1;
      repin_cached_fs(fs_addr, fs);
      EXPECT_GE(H5FS_close(f, fs), 0);
      EXPECT_GE(H5FS_delete(f, fs_addr), 0);
    }
  }

  // 5: H5FS_free error paths (lines 908, 924, 929, 952, 956, 969).
  {
    // 5a: Cached sinfo ring mismatch (908), xfree fail (924), dirty fail (929).
    {
      haddr_t fs_addr = HADDR_UNDEF;
      H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
      ASSERT_NE(fs, nullptr);
      ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x500, 64)->base, 0, nullptr,
                              nullptr),
                0);
      ASSERT_GE(H5FS_alloc_sect(f, fs), 0);
      ASSERT_EQ(fs->sinfo, nullptr);

      // Line 908: H5AC_protect(H5AC_FSPACE_SINFO) fails on ring mismatch.
      {
        H5AC_ring_t orig_ring = H5AC_RING_INV;
        H5AC_set_ring(H5AC_RING_MDFSM, &orig_ring);
        auto restore_ring =
            absl::MakeCleanup([&] { H5AC_set_ring(orig_ring, nullptr); });
        EXPECT_LT(H5FS_free(f, fs, /*free_file_space=*/false), 0);
      }

      // Line 924: H5MF_xfree(sinfo) fails with settled FSM ring.
      const haddr_t sinfo_addr = fs->sect_addr;
      const hsize_t sinfo_alloc_size = fs->alloc_sect_size;
      {
        auto settled = make_settled_guard();
        EXPECT_LT(H5FS_free(f, fs, /*free_file_space=*/true), 0);
      }
      ASSERT_NE(fs->sinfo, nullptr);
      EXPECT_GE(
          H5MF_xfree(f, H5FD_MEM_FSPACE_SINFO, sinfo_addr, sinfo_alloc_size),
          0);

      // Line 929: H5FS__dirty(fs) fails on unpinned fs after freeing sect_addr.
      ASSERT_GE(H5FS_alloc_sect(f, fs), 0);
      ASSERT_GE(H5AC_unpin_entry(fs), 0);
      EXPECT_LT(H5FS_free(f, fs, /*free_file_space=*/true), 0);
      repin_cached_fs(fs_addr, fs);
      EXPECT_GE(H5FS_close(f, fs), 0);
      EXPECT_GE(H5FS_delete(f, fs_addr), 0);
    }

    // 5b: Line 952 (protect hdr ring mismatch) & line 956 (unpin hdr fail).
    {
      haddr_t fs_addr = HADDR_UNDEF;
      H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
      ASSERT_NE(fs, nullptr);
      {
        H5AC_ring_t orig_ring = H5AC_RING_INV;
        H5AC_set_ring(H5AC_RING_MDFSM, &orig_ring);
        auto restore_ring =
            absl::MakeCleanup([&] { H5AC_set_ring(orig_ring, nullptr); });
        EXPECT_LT(H5FS_free(f, fs, /*free_file_space=*/false), 0);
      }

      ASSERT_GE(H5AC_unpin_entry(fs), 0);
      EXPECT_LT(H5FS_free(f, fs, /*free_file_space=*/false), 0);
      // H5FS_free left fs protected when H5AC_unpin_entry failed; re-pin and unprotect.
      ASSERT_GE(H5AC_pin_protected_entry(fs), 0);
      ASSERT_GE(
          H5AC_unprotect(f, H5AC_FSPACE_HDR, fs_addr, fs, H5AC__NO_FLAGS_SET),
          0);
      EXPECT_GE(H5FS_free(f, fs, /*free_file_space=*/true), 0);
      EXPECT_GE(H5FS_close(f, fs), 0);
    }

    // 5c: Line 969 (H5MF_xfree hdr failure on non-cached header address).
    {
      H5FS_t* fs = H5FS_create(f, nullptr, &cparam, 1, classes, &ctrl, 0, 0);
      ASSERT_NE(fs, nullptr);
      const haddr_t hdr_addr =
          H5MF_alloc(f, H5FD_MEM_FSPACE_HDR, fs->hdr_size);
      ASSERT_TRUE(H5_addr_defined(hdr_addr));
      fs->addr = hdr_addr;
      {
        auto settled = make_settled_guard();
        EXPECT_LT(H5FS_free(f, fs, /*free_file_space=*/true), 0);
      }
      EXPECT_GE(H5MF_xfree(f, H5FD_MEM_FSPACE_HDR, hdr_addr, fs->hdr_size), 0);
      EXPECT_GE(H5FS_close(f, fs), 0);
    }
  }
}

}  // namespace
}  // namespace hdf5
