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
// Tests free-space section tracking: sinfo creation/locking/unlocking,
// section addition, coalescing/merging, shrinking, extending, best-fit and
// aligned lookup/splitting, iteration, class transitions, VFD space settling,
// and error recovery paths (third_party/hdf5/src/H5FSsection.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FSsection.c

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with `-DNDEBUG`, which omits debug/stats fields from
// `H5C_cache_entry_t` (`H5AC_info_t`), the first member of `H5FS_t` and
// `H5FS_sinfo_t`. Match `NDEBUG` around the prelude and internal headers so
// struct offsets in C++ match `:hdf5`.
#ifndef NDEBUG
#define H5FSSECTION_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5FS_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FSpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5FSSECTION_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FSSECTION_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// Section class IDs registered on our test H5FS_t instances.
constexpr unsigned kClsSerialMerge = 0;   // Serializable, mergeable, splittable
constexpr unsigned kClsGhostSep = 1;      // Ghost + separate (no merge_list)
constexpr unsigned kClsSerialSym = 2;     // Serializable + MERGE_SYM + ADJUST_OK
constexpr unsigned kClsGhostMerge = 3;    // Ghost + mergeable + ADJUST_OK
constexpr unsigned kClsNoCallbacks = 4;   // Serializable, no merge/shrink/split

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

  H5FS_t* release() {
    f_ = nullptr;
    return std::exchange(fs_, nullptr);
  }

  H5FS_t* get() const { return fs_; }

 private:
  H5F_t* f_ = nullptr;
  H5FS_t* fs_ = nullptr;
};

struct TestSection {
  H5FS_section_info_t sect_info{};
  uint32_t tag = 0;
  TestSection** split_sink = nullptr;
};

struct TestOpsContext {
  bool fail_add = false;
  bool drop_on_add = false;
  bool fail_can_merge = false;
  bool fail_merge = false;
  hsize_t merge_vanish_size = 0;
  bool fail_can_shrink = false;
  bool fail_shrink = false;
  haddr_t shrink_eoa = HADDR_UNDEF;
  haddr_t next_eoa_after_first = HADDR_UNDEF;
  hsize_t partial_shrink_keep = 0;
  int shrink_calls = 0;
};

TestSection* MakeSection(unsigned type, haddr_t addr, hsize_t size,
                         uint32_t tag = 0,
                         H5FS_section_state_t state = H5FS_SECT_LIVE) {
  auto* s = static_cast<TestSection*>(std::calloc(1, sizeof(TestSection)));
  if (s != nullptr) {
    s->sect_info.addr = addr;
    s->sect_info.size = size;
    s->sect_info.type = type;
    s->sect_info.state = state;
    s->tag = tag;
  }
  return s;
}

// Holds an unlinked TestSection* cleanly across early ASSERT_* exits.
struct SectionDeleter {
  void operator()(TestSection* s) const { std::free(s); }
};
using UniqueSection = std::unique_ptr<TestSection, SectionDeleter>;

herr_t CbAdd(H5FS_section_info_t** sect, unsigned* flags, void* op_data) {
  if (flags != nullptr && (*flags & H5FS_ADD_DESERIALIZING) != 0) {
    // During cache deserialization, op_data points to H5FS_sinfo_cache_ud_t*.
    return 0;
  }
  auto* ctx = static_cast<TestOpsContext*>(op_data);
  if (ctx == nullptr) {
    return 0;
  }
  if (ctx->fail_add) {
    return -1;
  }
  if (ctx->drop_on_add) {
    std::free(*sect);
    *sect = nullptr;
    *flags &= ~static_cast<unsigned>(H5FS_ADD_RETURNED_SPACE);
    *flags |= H5FS_PAGE_END_NO_ADD;
  }
  return 0;
}

herr_t CbSerialize(const H5FS_section_class_t* cls,
                   const H5FS_section_info_t* sect, uint8_t* buf) {
  const auto* s = reinterpret_cast<const TestSection*>(sect);
  for (size_t i = 0; i < cls->serial_size && i < sizeof(s->tag); ++i) {
    buf[i] = static_cast<uint8_t>((s->tag >> (i * 8)) & 0xffU);
  }
  return 0;
}

H5FS_section_info_t* CbDeserialize(const H5FS_section_class_t* cls,
                                   const uint8_t* buf, haddr_t sect_addr,
                                   hsize_t sect_size,
                                   unsigned* /*des_flags*/) {
  uint32_t tag = 0;
  for (size_t i = 0; i < cls->serial_size && i < sizeof(tag); ++i) {
    tag |= static_cast<uint32_t>(buf[i]) << (i * 8);
  }
  TestSection* s =
      MakeSection(cls->type, sect_addr, sect_size, tag, H5FS_SECT_SERIALIZED);
  return s != nullptr ? &s->sect_info : nullptr;
}

htri_t CbCanMerge(const H5FS_section_info_t* sect1,
                  const H5FS_section_info_t* sect2, void* op_data) {
  auto* ctx = static_cast<TestOpsContext*>(op_data);
  if (ctx != nullptr && ctx->fail_can_merge) {
    return -1;
  }
  return sect1->addr + sect1->size == sect2->addr ? 1 : 0;
}

herr_t CbMerge(H5FS_section_info_t** sect1, H5FS_section_info_t* sect2,
               void* op_data) {
  auto* ctx = static_cast<TestOpsContext*>(op_data);
  if (ctx != nullptr && ctx->fail_merge) {
    return -1;
  }
  (*sect1)->size += sect2->size;
  std::free(sect2);
  if (ctx != nullptr && ctx->merge_vanish_size > 0 &&
      (*sect1)->size == ctx->merge_vanish_size) {
    std::free(*sect1);
    *sect1 = nullptr;
  }
  return 0;
}

htri_t CbCanShrink(const H5FS_section_info_t* sect, void* op_data) {
  auto* ctx = static_cast<TestOpsContext*>(op_data);
  if (ctx == nullptr) {
    return 0;
  }
  if (ctx->fail_can_shrink) {
    return -1;
  }
  if (ctx->shrink_eoa != HADDR_UNDEF &&
      sect->addr + sect->size == ctx->shrink_eoa) {
    return 1;
  }
  return 0;
}

herr_t CbShrink(H5FS_section_info_t** sect, void* op_data) {
  auto* ctx = static_cast<TestOpsContext*>(op_data);
  if (ctx != nullptr) {
    ++ctx->shrink_calls;
    if (ctx->fail_shrink) {
      return -1;
    }
    if (ctx->partial_shrink_keep > 0 &&
        (*sect)->size > ctx->partial_shrink_keep) {
      (*sect)->size = ctx->partial_shrink_keep;
      ctx->shrink_eoa -= (*sect)->size;
      ctx->partial_shrink_keep = 0;
      return 0;
    }
    ctx->shrink_eoa -= (*sect)->size;
    if (ctx->next_eoa_after_first != HADDR_UNDEF) {
      ctx->shrink_eoa = ctx->next_eoa_after_first;
      ctx->next_eoa_after_first = HADDR_UNDEF;
    }
  }
  std::free(*sect);
  *sect = nullptr;
  return 0;
}

herr_t CbFree(H5FS_section_info_t* sect) {
  std::free(sect);
  return 0;
}

herr_t CbFailFree(H5FS_section_info_t* /*sect*/) { return -1; }

herr_t CbValid(const H5FS_section_class_t* /*cls*/,
               const H5FS_section_info_t* /*sect*/) {
  return 0;
}

H5FS_section_info_t* CbSplit(H5FS_section_info_t* sect, hsize_t frag_size) {
  auto* orig = reinterpret_cast<TestSection*>(sect);
  TestSection* frag =
      MakeSection(orig->sect_info.type, orig->sect_info.addr, frag_size,
                  orig->tag, orig->sect_info.state);
  if (orig->split_sink != nullptr) {
    *orig->split_sink = frag;
  }
  orig->sect_info.addr += frag_size;
  orig->sect_info.size -= frag_size;
  return frag != nullptr ? &frag->sect_info : nullptr;
}

H5FS_section_class_t MakeClass0SerialMerge(size_t serial_size = 4) {
  return H5FS_section_class_t{
      /*type=*/kClsSerialMerge,
      /*serial_size=*/serial_size,
      /*flags=*/H5FS_CLS_ADJUST_OK,
      /*cls_private=*/nullptr,
      /*init_cls=*/nullptr,
      /*term_cls=*/nullptr,
      /*add=*/CbAdd,
      /*serialize=*/CbSerialize,
      /*deserialize=*/CbDeserialize,
      /*can_merge=*/CbCanMerge,
      /*merge=*/CbMerge,
      /*can_shrink=*/CbCanShrink,
      /*shrink=*/CbShrink,
      /*free=*/CbFree,
      /*valid=*/CbValid,
      /*split=*/CbSplit,
      /*debug=*/nullptr,
  };
}

constexpr H5FS_section_class_t kClass1GhostSep = {
    /*type=*/kClsGhostSep,
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
    /*free=*/CbFree,
    /*valid=*/nullptr,
    /*split=*/CbSplit,
    /*debug=*/nullptr,
};

constexpr H5FS_section_class_t kClass2SerialSym = {
    /*type=*/kClsSerialSym,
    /*serial_size=*/0,
    /*flags=*/H5FS_CLS_MERGE_SYM | H5FS_CLS_ADJUST_OK,
    /*cls_private=*/nullptr,
    /*init_cls=*/nullptr,
    /*term_cls=*/nullptr,
    /*add=*/nullptr,
    /*serialize=*/nullptr,
    /*deserialize=*/CbDeserialize,
    /*can_merge=*/CbCanMerge,
    /*merge=*/CbMerge,
    /*can_shrink=*/nullptr,
    /*shrink=*/nullptr,
    /*free=*/CbFree,
    /*valid=*/nullptr,
    /*split=*/CbSplit,
    /*debug=*/nullptr,
};

constexpr H5FS_section_class_t kClass3GhostMerge = {
    /*type=*/kClsGhostMerge,
    /*serial_size=*/0,
    /*flags=*/H5FS_CLS_GHOST_OBJ | H5FS_CLS_ADJUST_OK,
    /*cls_private=*/nullptr,
    /*init_cls=*/nullptr,
    /*term_cls=*/nullptr,
    /*add=*/nullptr,
    /*serialize=*/nullptr,
    /*deserialize=*/nullptr,
    /*can_merge=*/CbCanMerge,
    /*merge=*/CbMerge,
    /*can_shrink=*/nullptr,
    /*shrink=*/nullptr,
    /*free=*/CbFree,
    /*valid=*/nullptr,
    /*split=*/CbSplit,
    /*debug=*/nullptr,
};

constexpr H5FS_section_class_t kClass4NoCallbacks = {
    /*type=*/kClsNoCallbacks,
    /*serial_size=*/2,
    /*flags=*/0,
    /*cls_private=*/nullptr,
    /*init_cls=*/nullptr,
    /*term_cls=*/nullptr,
    /*add=*/nullptr,
    /*serialize=*/CbSerialize,
    /*deserialize=*/CbDeserialize,
    /*can_merge=*/nullptr,
    /*merge=*/nullptr,
    /*can_shrink=*/nullptr,
    /*shrink=*/nullptr,
    /*free=*/CbFree,
    /*valid=*/nullptr,
    /*split=*/nullptr,
    /*debug=*/nullptr,
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5FS_create_t MakeDefaultCreateParams(hsize_t max_sect_size = 65536) {
  H5FS_create_t cparam{};
  cparam.client = H5FS_CLIENT_FHEAP_ID;
  cparam.shrink_percent = 20;
  cparam.expand_percent = 120;
  cparam.max_sect_addr = 32;
  cparam.max_sect_size = max_sect_size;
  return cparam;
}

struct ClassBundle {
  explicit ClassBundle(size_t c0_serial_size = 4)
      : c0(MakeClass0SerialMerge(c0_serial_size)) {}

  H5FS_section_class_t c0;
  H5FS_section_class_t c1 = kClass1GhostSep;
  H5FS_section_class_t c2 = kClass2SerialSym;
  H5FS_section_class_t c3 = kClass3GhostMerge;
  H5FS_section_class_t c4 = kClass4NoCallbacks;
  const H5FS_section_class_t* ptrs[5] = {&c0, &c1, &c2, &c3, &c4};
};

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

TEST(H5FSsectionTest, SinfoNewLinkUnlinkAndSerializeSizeTransitions) {
  ScopedFile file = CreateInMemoryFile("h5fs_sinfo_new.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(nullptr, f);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  ClassBundle bundle;
  const H5FS_create_t cparam = MakeDefaultCreateParams();
  ScopedFspace fs(f, H5FS_create(f, /*fs_addr=*/nullptr, &cparam, /*nclasses=*/5,
                                 bundle.ptrs, /*cls_init_udata=*/nullptr,
                                 /*alignment=*/0, /*threshold=*/0));
  ASSERT_NE(nullptr, fs.get());

  // 1. Direct H5FS__sinfo_new initializes nbins and offset/length encoding.
  H5FS_sinfo_t* sinfo = H5FS__sinfo_new(f, fs.get());
  ASSERT_NE(nullptr, sinfo);
  EXPECT_EQ(fs.get(), sinfo->fspace);
  EXPECT_EQ(sinfo, fs.get()->sinfo);
  EXPECT_EQ(2u, fs.get()->rc);
  EXPECT_GT(sinfo->nbins, 0u);
  EXPECT_EQ(4u, sinfo->sect_off_size);

  // 2. Add two sections of the same size (1 serializable, 1 ghost) plus a
  //    separate ghost section and another serializable section of same size.
  TestSection* s1 = MakeSection(kClsSerialMerge, 1000, 128, 1);
  TestSection* s2 = MakeSection(kClsSerialMerge, 2000, 128, 2);
  TestSection* g_same_size = MakeSection(kClsGhostMerge, 3000, 128, 3);
  TestSection* g_sep = MakeSection(kClsGhostSep, 4000, 256, 4);
  ASSERT_NE(nullptr, s1);
  ASSERT_NE(nullptr, s2);
  ASSERT_NE(nullptr, g_same_size);
  ASSERT_NE(nullptr, g_sep);

  TestOpsContext ctx;
  // Adding with H5FS_ADD_DESERIALIZING skips serialize-size update and dirtying.
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &s1->sect_info, H5FS_ADD_DESERIALIZING,
                          &ctx, nullptr),
            0);
  EXPECT_EQ(0u, fs.get()->sect_size);
  EXPECT_FALSE(sinfo->dirty);

  ASSERT_GE(H5FS_sect_add(f, fs.get(), &s2->sect_info, 0, &ctx, nullptr), 0);
  ASSERT_GE(
      H5FS_sect_add(f, fs.get(), &g_same_size->sect_info, 0, &ctx, nullptr), 0);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &g_sep->sect_info, 0, &ctx, nullptr), 0);
  EXPECT_GT(fs.get()->sect_size, sinfo->sect_prefix_size);

  // Check H5FS_sect_stats with both non-NULL and partial NULL pointers.
  hsize_t tot_space = 0;
  hsize_t nsects = 0;
  ASSERT_GE(H5FS_sect_stats(fs.get(), &tot_space, &nsects), 0);
  EXPECT_EQ(640u, tot_space);
  EXPECT_EQ(4u, nsects);
  EXPECT_GE(H5FS_sect_stats(fs.get(), &tot_space, nullptr), 0);
  EXPECT_GE(H5FS_sect_stats(fs.get(), nullptr, &nsects), 0);
  EXPECT_GE(H5FS_sect_stats(fs.get(), nullptr, nullptr), 0);

  // 3. Remove ghost section sharing size=128 node -> ghost_count drops to 0
  //    while serial sections remain in the same size node.
  ASSERT_GE(H5FS_sect_remove(f, fs.get(), &g_same_size->sect_info), 0);
  std::free(g_same_size);

  // Remove first of two serial sections of size=128 -> serial_count 2 -> 1.
  ASSERT_GE(H5FS_sect_remove(f, fs.get(), &s1->sect_info), 0);
  std::free(s1);

  // Remove second serial section of size=128 -> node destroyed,
  // serial_sect_count hits 0 so H5FS__sect_serialize_size resets sect_size.
  ASSERT_GE(H5FS_sect_remove(f, fs.get(), &s2->sect_info), 0);
  std::free(s2);
  EXPECT_EQ(0u, fs.get()->serial_sect_count);
  EXPECT_EQ(sinfo->sect_prefix_size, fs.get()->sect_size);

  // Remove separate ghost section (not in merge_list).
  ASSERT_GE(H5FS_sect_remove(f, fs.get(), &g_sep->sect_info), 0);
  std::free(g_sep);
  ASSERT_GE(H5FS_sect_stats(fs.get(), &tot_space, &nsects), 0);
  EXPECT_EQ(0u, tot_space);
  EXPECT_EQ(0u, nsects);
}

TEST(H5FSsectionTest, AddMergeChainCrossClassSymDropAndCascadingShrink) {
  ScopedFile file = CreateInMemoryFile("h5fs_merge_shrink.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(nullptr, f);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  ClassBundle bundle;
  const H5FS_create_t cparam = MakeDefaultCreateParams();
  ScopedFspace fs(f, H5FS_create(f, /*fs_addr=*/nullptr, &cparam, /*nclasses=*/5,
                                 bundle.ptrs, /*cls_init_udata=*/nullptr,
                                 /*alignment=*/0, /*threshold=*/0));
  ASSERT_NE(nullptr, fs.get());

  TestOpsContext ctx;

  // 1. Add separate ghost section while merge_list is still NULL.
  TestSection* g_sep = MakeSection(kClsGhostSep, 100, 32);
  ASSERT_NE(nullptr, g_sep);
  bool merged_or_shrunk = true;
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &g_sep->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, &merged_or_shrunk),
            0);
  EXPECT_FALSE(merged_or_shrunk);

  // 2. Add [1000, 1064) (Class 0) and [1128, 1192) (Class 3, ghost mergeable),
  //    then insert bridging [1064, 1128) (Class 0): merges left then right
  //    across non-sym classes.
  TestSection* left = MakeSection(kClsSerialMerge, 1000, 64);
  TestSection* right = MakeSection(kClsGhostMerge, 1128, 64);
  TestSection* bridge = MakeSection(kClsSerialMerge, 1064, 64);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &left->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &right->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &bridge->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, &merged_or_shrunk),
            0);
  EXPECT_FALSE(merged_or_shrunk);
  EXPECT_EQ(192u, left->sect_info.size);

  // 3. Insert [936, 1000) BEFORE [1000, 1192) when less_sect_node == NULL ->
  //    exercises H5SL_above greater_sect_node path.
  TestSection* pre_left = MakeSection(kClsSerialMerge, 936, 64);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &pre_left->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  EXPECT_EQ(256u, pre_left->sect_info.size);

  // 4. Merge symmetry check: Class 2 (H5FS_CLS_MERGE_SYM) does NOT merge with
  //    adjoining Class 0 on either side, only with another Class 2.
  TestSection* c0_bound = MakeSection(kClsSerialMerge, 1282, 18);
  TestSection* sym_a = MakeSection(kClsSerialSym, 1250, 32);
  TestSection* sym_b = MakeSection(kClsSerialSym, 1218, 32);
  TestSection* sym_c = MakeSection(kClsSerialSym, 1350, 32);
  TestSection* c0_after_sym = MakeSection(kClsSerialMerge, 1382, 32);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &c0_bound->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &sym_a->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  EXPECT_EQ(32u, sym_a->sect_info.size);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &sym_b->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  // sym_b merged right into [1218, 1282) (sym_a freed) and stopped at c0_bound.
  EXPECT_EQ(64u, sym_b->sect_info.size);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &sym_c->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &c0_after_sym->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  EXPECT_EQ(32u, sym_c->sect_info.size);
  EXPECT_EQ(32u, c0_after_sym->sect_info.size);

  // Class 4 has can_merge == nullptr and does not merge with adjoining Class 0.
  TestSection* no_cb = MakeSection(kClsNoCallbacks, 872, 64);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &no_cb->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);

  // 5. Exercise merge setting *sect = nullptr (less_sect_node & greater_sect):
  //    set merge_vanish_size = 128 for [2000, 2064) + [2064, 2128).
  ctx.merge_vanish_size = 128;
  TestSection* van_l = MakeSection(kClsSerialMerge, 2000, 64);
  TestSection* van_r = MakeSection(kClsSerialMerge, 2064, 64);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &van_l->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &van_r->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, &merged_or_shrunk),
            0);
  EXPECT_TRUE(merged_or_shrunk);

  // Same vanish via greater_sect_node (insert left of existing right).
  TestSection* van_r2 = MakeSection(kClsSerialMerge, 2564, 64);
  TestSection* van_l2 = MakeSection(kClsSerialMerge, 2500, 64);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &van_r2->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &van_l2->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, &merged_or_shrunk),
            0);
  EXPECT_TRUE(merged_or_shrunk);
  ctx.merge_vanish_size = 0;

  // 6. Exercise cls->add dropping section via H5FS_PAGE_END_NO_ADD.
  ctx.drop_on_add = true;
  TestSection* dropped = MakeSection(kClsSerialMerge, 3000, 16);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &dropped->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, &merged_or_shrunk),
            0);
  EXPECT_TRUE(merged_or_shrunk);
  ctx.drop_on_add = false;

  // 7. Partial shrink and cascading shrink (remove_sect = true in merge loop):
  //    Keep existing sections at < 1256; add [4000, 4100), [4100, 4200),
  //    then shrink_eoa = 4300 and add [4200, 4300).
  ctx.shrink_eoa = 4300;
  ctx.partial_shrink_keep = 32;  // First pass partial shrink -> [4200, 4232).
  TestSection* pshrink = MakeSection(kClsSerialMerge, 4200, 100);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &pshrink->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, &merged_or_shrunk),
            0);
  EXPECT_FALSE(merged_or_shrunk);
  EXPECT_EQ(32u, pshrink->sect_info.size);
  ctx.partial_shrink_keep = 0;

  // Remove partial-shrunk section, verify try_shrink_eoa returns 0 on a
  // non-matching EOA, then shrink a tail section via H5FS_sect_try_shrink_eoa.
  ASSERT_GE(H5FS_sect_remove(f, fs.get(), &pshrink->sect_info), 0);
  std::free(pshrink);

  EXPECT_EQ(0, H5FS_sect_try_shrink_eoa(f, fs.get(), &ctx));

  TestSection* tail = MakeSection(kClsSerialMerge, 4000, 100);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &tail->sect_info, 0, &ctx, nullptr), 0);
  ctx.shrink_eoa = 9999;
  EXPECT_EQ(0, H5FS_sect_try_shrink_eoa(f, fs.get(), &ctx));

  ctx.shrink_eoa = 4100;
  EXPECT_GT(H5FS_sect_try_shrink_eoa(f, fs.get(), &ctx), 0);
  EXPECT_EQ(4000u, ctx.shrink_eoa);
}

TEST(H5FSsectionTest, CascadingShrinkAndTryShrinkEoaEdgeCases) {
  ScopedFile file = CreateInMemoryFile("h5fs_cascade_shrink.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(nullptr, f);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  // Custom shrink step via ctx.next_eoa_after_first drops shrink_eoa to the next
  // threshold after absorbing an intervening aggregator block, exercising the
  // multi-pass `remove_sect = true` branch in H5FS__sect_merge.
  H5FS_section_class_t c0 = MakeClass0SerialMerge(0);
  const H5FS_section_class_t* classes[] = {&c0, &kClass4NoCallbacks};
  const H5FS_create_t cparam = MakeDefaultCreateParams();
  ScopedFspace fs(f, H5FS_create(f, /*fs_addr=*/nullptr, &cparam, /*nclasses=*/2,
                                 classes, /*cls_init_udata=*/nullptr, 0, 0));
  ASSERT_NE(nullptr, fs.get());

  TestOpsContext ctx;
  // Try shrink when merge_list is NULL -> false.
  EXPECT_EQ(0, H5FS_sect_try_shrink_eoa(f, fs.get(), &ctx));

  // Add Class 4 (can_shrink == nullptr) at [500, 564) -> try_shrink_eoa = 0.
  TestSection* s_nocb = MakeSection(1, 500, 64);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &s_nocb->sect_info, 0, &ctx, nullptr),
            0);
  EXPECT_EQ(0, H5FS_sect_try_shrink_eoa(f, fs.get(), &ctx));

  // Add Class 0 [1000, 1100) without RETURNED_SPACE, then add [1200, 1300)
  // with shrink_eoa = 1300 and next_eoa_after_first = 1100 -> shrinks
  // [1200, 1300), then cascades to remove & shrink [1000, 1100), then stops
  // cleanly on [500, 564) (setting *sect = NULL via line 1283-1284).
  TestSection* s1 = MakeSection(0, 1000, 100);
  TestSection* s2 = MakeSection(0, 1200, 100);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &s1->sect_info, 0, &ctx, nullptr), 0);

  ctx.shrink_eoa = 1300;
  ctx.next_eoa_after_first = 1100;
  bool merged_or_shrunk = false;
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &s2->sect_info, H5FS_ADD_RETURNED_SPACE,
                          &ctx, &merged_or_shrunk),
            0);
  EXPECT_TRUE(merged_or_shrunk);
  EXPECT_EQ(2, ctx.shrink_calls);
  EXPECT_EQ(1000u, ctx.shrink_eoa);
  EXPECT_EQ(1u, fs.get()->tot_sect_count);
}

TEST(H5FSsectionTest, TryExtendAndTryMergeAllBranches) {
  ScopedFile file = CreateInMemoryFile("h5fs_extend_merge.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(nullptr, f);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  ClassBundle bundle;
  const H5FS_create_t cparam = MakeDefaultCreateParams();
  ScopedFspace fs(f, H5FS_create(f, /*fs_addr=*/nullptr, &cparam, /*nclasses=*/5,
                                 bundle.ptrs, nullptr, 0, 0));
  ASSERT_NE(nullptr, fs.get());

  TestOpsContext ctx;

  // 1. Empty manager -> try_extend returns false.
  EXPECT_EQ(0, H5FS_sect_try_extend(f, fs.get(), 100, 50, 20, 0, &ctx));

  // Add [1000, 1100) (Class 0) and [2000, 2064) (Class 2, add == nullptr).
  TestSection* s1 = MakeSection(kClsSerialMerge, 1000, 100);
  TestSection* s2 = MakeSection(kClsSerialSym, 2000, 64);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &s1->sect_info, 0, &ctx, nullptr), 0);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &s2->sect_info, 0, &ctx, nullptr), 0);

  // 2. Miss cases: no section > addr, non-adjoining, too small.
  EXPECT_EQ(0, H5FS_sect_try_extend(f, fs.get(), 3000, 10, 10, 0, &ctx));
  EXPECT_EQ(0, H5FS_sect_try_extend(f, fs.get(), 900, 50, 10, 0, &ctx));
  EXPECT_EQ(0, H5FS_sect_try_extend(f, fs.get(), 900, 100, 200, 0, &ctx));

  // 3. Partial extend with cls->add != nullptr (keeps section).
  EXPECT_GT(H5FS_sect_try_extend(f, fs.get(), 900, 100, 40, 0, &ctx), 0);
  EXPECT_EQ(1040u, s1->sect_info.addr);
  EXPECT_EQ(60u, s1->sect_info.size);

  // 4. Partial extend with cls->add == nullptr (Class 2).
  EXPECT_GT(H5FS_sect_try_extend(f, fs.get(), 1900, 100, 32, 0, &ctx), 0);
  EXPECT_EQ(2032u, s2->sect_info.addr);
  EXPECT_EQ(32u, s2->sect_info.size);

  // 5. Partial extend where cls->add drops adjusted section (sets *sect=NULL).
  ctx.drop_on_add = true;
  EXPECT_GT(H5FS_sect_try_extend(f, fs.get(), 900, 140, 20, 0, &ctx), 0);
  ctx.drop_on_add = false;

  // 6. Exact extend frees section.
  EXPECT_GT(H5FS_sect_try_extend(f, fs.get(), 1900, 132, 32, 0, &ctx), 0);
  EXPECT_EQ(0u, fs.get()->tot_sect_count);

  // 7. H5FS_sect_try_merge: unmerged, merged, and shrunk away.
  TestSection* anchor = MakeSection(kClsSerialMerge, 5000, 100);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &anchor->sect_info, 0, &ctx, nullptr),
            0);

  // Non-adjoining block is not merged and NOT linked (still owned by caller).
  UniqueSection isolated(MakeSection(kClsSerialMerge, 6000, 50));
  EXPECT_EQ(0, H5FS_sect_try_merge(f, fs.get(), &isolated->sect_info, 0, &ctx));

  // Adjoining block merges into anchor and re-links updated section.
  TestSection* adjoin = MakeSection(kClsSerialMerge, 5100, 50);
  EXPECT_GT(H5FS_sect_try_merge(f, fs.get(), &adjoin->sect_info, 0, &ctx), 0);
  EXPECT_EQ(150u, anchor->sect_info.size);

  // Shrinking block at EOA shrinks away to NULL and returns true.
  ctx.shrink_eoa = 5150;
  TestSection* eoa_sect = MakeSection(kClsSerialMerge, 5150, 50);
  // Note: [5150, 5200) will adjoin [5000, 5150) -> merge -> if shrink_eoa=5200
  // it shrinks the whole [5000, 5200) away!
  ctx.shrink_eoa = 5200;
  EXPECT_GT(H5FS_sect_try_merge(f, fs.get(), &eoa_sect->sect_info, 0, &ctx), 0);
  EXPECT_EQ(0u, fs.get()->tot_sect_count);
}

TEST(H5FSsectionTest, FindBestFitAndAlignedSplitAcrossBinsAndSizes) {
  ScopedFile file = CreateInMemoryFile("h5fs_find_align.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(nullptr, f);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  ClassBundle bundle;
  const H5FS_create_t cparam = MakeDefaultCreateParams();

  // 1. Unaligned best-fit lookup across multiple bins.
  {
    ScopedFspace fs(f, H5FS_create(f, nullptr, &cparam, 5, bundle.ptrs, nullptr,
                                   /*alignment=*/0, /*threshold=*/0));
    ASSERT_NE(nullptr, fs.get());

    H5FS_section_info_t* found = nullptr;
    EXPECT_EQ(0, H5FS_sect_find(f, fs.get(), 32, &found));

    // Add size 30 (bin 4), size 200 (bin 7), size 600 (bin 9).
    TestSection* s30 = MakeSection(kClsSerialMerge, 1000, 30);
    TestSection* s200 = MakeSection(kClsSerialMerge, 2000, 200);
    TestSection* s600 = MakeSection(kClsSerialMerge, 3000, 600);
    ASSERT_GE(H5FS_sect_add(f, fs.get(), &s30->sect_info, 0, nullptr, nullptr),
              0);
    ASSERT_GE(H5FS_sect_add(f, fs.get(), &s200->sect_info, 0, nullptr, nullptr),
              0);
    ASSERT_GE(H5FS_sect_add(f, fs.get(), &s600->sect_info, 0, nullptr, nullptr),
              0);

    // Request 100 (bin 6 is empty -> advances bin++ to bin 7 -> gets s200).
    EXPECT_GT(H5FS_sect_find(f, fs.get(), 100, &found), 0);
    ASSERT_EQ(&s200->sect_info, found);
    std::free(found);

    // Request 1000 -> exceeds all sections -> false.
    found = nullptr;
    EXPECT_EQ(0, H5FS_sect_find(f, fs.get(), 1000, &found));
  }

  // 2. Aligned lookup with alignment=64, threshold=32.
  {
    ScopedFspace fs(f, H5FS_create(f, nullptr, &cparam, 5, bundle.ptrs, nullptr,
                                   /*alignment=*/64, /*threshold=*/32));
    ASSERT_NE(nullptr, fs.get());

    // Add:
    // - Class 4 (split == nullptr) at aligned 6400 size 70 (skipped for align)
    // - Class 0 at misaligned 7009 size 70 (7009%64=33, frag=31, 70 < 40+31)
    // - Class 0 at misaligned 7200 size 80 (7200%64=32, frag=32, 80 >= 40+32)
    // - Class 0 at aligned 8192 size 128 (bin 7, exact aligned)
    TestSection* s_nosplit = MakeSection(kClsNoCallbacks, 6400, 70);
    TestSection* s_small = MakeSection(kClsSerialMerge, 7009, 70);
    TestSection* s_split = MakeSection(kClsSerialMerge, 7200, 80);
    TestSection* s_exact = MakeSection(kClsSerialMerge, 8192, 128);
    ASSERT_GE(
        H5FS_sect_add(f, fs.get(), &s_nosplit->sect_info, 0, nullptr, nullptr),
        0);
    ASSERT_GE(
        H5FS_sect_add(f, fs.get(), &s_small->sect_info, 0, nullptr, nullptr),
        0);
    ASSERT_GE(
        H5FS_sect_add(f, fs.get(), &s_split->sect_info, 0, nullptr, nullptr),
        0);
    ASSERT_GE(
        H5FS_sect_add(f, fs.get(), &s_exact->sect_info, 0, nullptr, nullptr),
        0);

    // Request 40 (>= threshold 32): skips s_nosplit (split==NULL) and s_small
    // (size < 40 + frag), splits s_split at 7200 -> frag [7200, 7232) re-linked
    // and aligned [7232, 7280) returned!
    H5FS_section_info_t* found = nullptr;
    EXPECT_GT(H5FS_sect_find(f, fs.get(), 40, &found), 0);
    ASSERT_NE(nullptr, found);
    EXPECT_EQ(0u, found->addr % 64);
    EXPECT_EQ(7232u, found->addr);
    EXPECT_EQ(48u, found->size);
    std::free(found);

    // Request 100 (>= threshold 32): scans bin 6 (fails size), advances to
    // bin 7, finds aligned s_exact (mis_align == 0 -> no split).
    found = nullptr;
    EXPECT_GT(H5FS_sect_find(f, fs.get(), 100, &found), 0);
    ASSERT_EQ(&s_exact->sect_info, found);
    std::free(found);

    // Request 20 (< threshold 32): bypasses alignment and finds bin 4/5 frag!
    found = nullptr;
    EXPECT_GT(H5FS_sect_find(f, fs.get(), 20, &found), 0);
    ASSERT_NE(nullptr, found);
    EXPECT_EQ(7200u, found->addr);
    EXPECT_EQ(32u, found->size);
    std::free(found);

    // Request 200 (>= threshold 32): nothing big enough -> false.
    EXPECT_EQ(0, H5FS_sect_find(f, fs.get(), 200, &found));
  }
}

TEST(H5FSsectionTest, ChangeClassIterateAndPersistedSinfoLockUpgrade) {
  ScopedFile file = CreateInMemoryFile("h5fs_persist_upgrade.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(nullptr, f);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  ClassBundle bundle;
  const H5FS_create_t cparam = MakeDefaultCreateParams();
  haddr_t fs_addr = HADDR_UNDEF;
  ScopedFspace fs(f, H5FS_create(f, &fs_addr, &cparam, 5, bundle.ptrs, nullptr,
                                 0, 0));
  ASSERT_NE(nullptr, fs.get());
  ASSERT_NE(HADDR_UNDEF, fs_addr);

  // 1. Empty iterate succeeds without locking sinfo.
  auto collect_cb = [](H5FS_section_info_t* sect, void* op_data) -> herr_t {
    auto* addrs = static_cast<std::vector<haddr_t>*>(op_data);
    addrs->push_back(sect->addr);
    return 0;
  };
  std::vector<haddr_t> addrs;
  EXPECT_GE(H5FS_sect_iterate(f, fs.get(), collect_cb, &addrs), 0);
  EXPECT_TRUE(addrs.empty());

  // 2. Test all H5FS_sect_change_class transitions on size=128 nodes:
  //    Add two Class 1 (Ghost+Sep) and one Class 0 (Serial+Mergeable) at 128.
  TestSection* gs1 = MakeSection(kClsGhostSep, 1000, 128);
  TestSection* gs2 = MakeSection(kClsGhostSep, 2000, 128);
  TestSection* sm1 = MakeSection(kClsSerialMerge, 3000, 128);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &gs1->sect_info, 0, nullptr, nullptr),
            0);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &gs2->sect_info, 0, nullptr, nullptr),
            0);

  // Convert gs1 (Ghost+Sep) -> Serial+Mergeable when merge_list == NULL and
  // serial_count == 0 -> creates merge_list, flips serial_size_count 0->1.
  ASSERT_GE(
      H5FS_sect_change_class(f, fs.get(), &gs1->sect_info, kClsSerialMerge), 0);
  EXPECT_EQ(1u, fs.get()->serial_sect_count);
  EXPECT_EQ(1u, fs.get()->ghost_sect_count);

  // Convert gs2 (Ghost+Sep) -> Serial+Mergeable while merge_list != NULL and
  // serial_count == 1 (already >0), ghost_count drops 1 -> 0!
  ASSERT_GE(
      H5FS_sect_change_class(f, fs.get(), &gs2->sect_info, kClsSerialMerge), 0);
  EXPECT_EQ(2u, fs.get()->serial_sect_count);
  EXPECT_EQ(0u, fs.get()->ghost_sect_count);

  // Convert gs1 Serial+Mergeable -> Ghost+Sep (serial 2->1, ghost 0->1).
  ASSERT_GE(
      H5FS_sect_change_class(f, fs.get(), &gs1->sect_info, kClsGhostSep), 0);
  // Convert gs2 Serial+Mergeable -> Ghost+Sep (serial 1->0, ghost 1->2).
  ASSERT_GE(
      H5FS_sect_change_class(f, fs.get(), &gs2->sect_info, kClsGhostSep), 0);
  EXPECT_EQ(0u, fs.get()->serial_sect_count);
  EXPECT_EQ(2u, fs.get()->ghost_sect_count);

  // Same ghost/separate flags, different serial_size (Class 0 <-> Class 2).
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &sm1->sect_info, 0, nullptr, nullptr),
            0);
  ASSERT_GE(
      H5FS_sect_change_class(f, fs.get(), &sm1->sect_info, kClsSerialSym), 0);
  ASSERT_GE(
      H5FS_sect_change_class(f, fs.get(), &sm1->sect_info, kClsSerialMerge), 0);

  // Remove gs1, gs2; keep two Class 0 sections at 3000 (128) and 4000 (256).
  ASSERT_GE(H5FS_sect_remove(f, fs.get(), &gs1->sect_info), 0);
  std::free(gs1);
  ASSERT_GE(H5FS_sect_remove(f, fs.get(), &gs2->sect_info), 0);
  std::free(gs2);

  TestSection* sm2 = MakeSection(kClsSerialMerge, 4000, 256, 77);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &sm2->sect_info, 0, nullptr, nullptr),
            0);

  // 3. Allocate sinfo in cache (fspace->sinfo becomes NULL, sect_addr defined),
  //    then close & reopen in the same open file and also flush to real space!
  ASSERT_GE(H5FS_alloc_sect(f, fs.get()), 0);
  EXPECT_EQ(nullptr, fs.get()->sinfo);
  EXPECT_NE(HADDR_UNDEF, fs.get()->sect_addr);

  // 3a. Read-only iterate on cached sinfo, then unlock unmodified.
  addrs.clear();
  EXPECT_GE(H5FS_sect_iterate(f, fs.get(), collect_cb, &addrs), 0);
  EXPECT_THAT(addrs, ElementsAre(3000u, 4000u));
  EXPECT_EQ(nullptr, fs.get()->sinfo);

  // 3b. Modify cached sinfo WITHOUT changing sect_size (extend adjusts
  //     [4000, 4256) -> [4032, 4224): same count, same size count!).
  EXPECT_GT(H5FS_sect_try_extend(f, fs.get(), 3900, 100, 32, 0, nullptr), 0);
  EXPECT_EQ(nullptr, fs.get()->sinfo);

  // 3c. Iterate RO on cached sinfo and call H5FS_sect_remove inside callback:
  //     triggers RO->RW upgrade in H5FS__sinfo_lock AND outer unmodified unlock
  //     seeing sinfo_modified==true and marking header dirty (line 478)!
  struct RemoveFirstUd {
    H5F_t* f;
    H5FS_t* fs;
    bool removed = false;
  } rf_ud{f, fs.get(), false};
  auto remove_first_cb = [](H5FS_section_info_t* sect, void* op_data) -> herr_t {
    auto* ud = static_cast<RemoveFirstUd*>(op_data);
    if (!ud->removed && sect->addr == 3000) {
      ud->removed = true;
      if (H5FS_sect_remove(ud->f, ud->fs, sect) < 0) {
        return -1;
      }
      std::free(sect);
    }
    return 0;
  };
  EXPECT_GE(H5FS_sect_iterate(f, fs.get(), remove_first_cb, &rf_ud), 0);
  EXPECT_TRUE(rf_ud.removed);

  // 3d. Re-close fs so H5FS_close allocates sect_addr in tmp space (because
  //     H5F_USE_TMP_SPACE(f) is true), then H5FS_open and add a section ->
  //     H5FS__sinfo_unlock sees H5F_IS_TMP_ADDR(f, old_sect_addr) == true!
  ASSERT_GE(H5FS_close(f, fs.release()), 0);
  fs.reset(f, H5FS_open(f, fs_addr, 5, bundle.ptrs, nullptr, 0, 0));
  ASSERT_NE(nullptr, fs.get());
  if (H5F_USE_TMP_SPACE(f)) {
    EXPECT_TRUE(H5F_IS_TMP_ADDR(f, fs.get()->sect_addr));
  }
  TestSection* extra = MakeSection(kClsSerialMerge, 9000, 64, 99);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &extra->sect_info, 0, nullptr, nullptr),
            0);

  // 4. Flush to disk, snapshot image, reopen RO, and verify persisted sections.
  fs.reset();
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  std::vector<uint8_t> image = SnapshotFileImage(file.get());
  ASSERT_FALSE(image.empty());

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(H5Fopen("h5fs_ro.h5", H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(ro_file.valid());
  H5F_t* rf = GetH5File(ro_file.get());
  ASSERT_NE(nullptr, rf);

  ScopedFspace ro_fs(rf, H5FS_open(rf, fs_addr, 5, bundle.ptrs, nullptr, 0, 0));
  ASSERT_NE(nullptr, ro_fs.get());
  addrs.clear();
  EXPECT_GE(H5FS_sect_iterate(rf, ro_fs.get(), collect_cb, &addrs), 0);
  EXPECT_THAT(addrs, ElementsAre(9000u, 4032u));
}

TEST(H5FSsectionTest, ErrorAndRollbackPathsLeaveStateClean) {
  ScopedFile file = CreateInMemoryFile("h5fs_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(nullptr, f);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  H5FS_section_class_t c0 = MakeClass0SerialMerge(4);
  const H5FS_section_class_t c_fail_free = {
      /*type=*/1,
      /*serial_size=*/0,
      /*flags=*/H5FS_CLS_MERGE_SYM | H5FS_CLS_ADJUST_OK,
      /*cls_private=*/nullptr,
      /*init_cls=*/nullptr,
      /*term_cls=*/nullptr,
      /*add=*/nullptr,
      /*serialize=*/nullptr,
      /*deserialize=*/CbDeserialize,
      /*can_merge=*/CbCanMerge,
      /*merge=*/CbMerge,
      /*can_shrink=*/nullptr,
      /*shrink=*/nullptr,
      /*free=*/CbFailFree,
      /*valid=*/nullptr,
      /*split=*/CbSplit,
      /*debug=*/nullptr,
  };
  const H5FS_section_class_t* classes[] = {&c0, &c_fail_free};

  const H5FS_create_t cparam = MakeDefaultCreateParams();
  ScopedFspace fs(f, H5FS_create(f, nullptr, &cparam, 2, classes, nullptr, 0, 0));
  ASSERT_NE(nullptr, fs.get());

  TestOpsContext ctx;
  ScopedErrorSilence silence;

  // 1. Add section into bin 7 (size 200 at addr 1000).
  TestSection* base = MakeSection(0, 1000, 200);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &base->sect_info, 0, &ctx, nullptr), 0);

  // 2. H5FS_sect_remove errors: empty bin, missing size in non-empty bin,
  //    missing addr in existing size node.
  UniqueSection bad_empty_bin(MakeSection(0, 1000, 16));
  EXPECT_LT(H5FS_sect_remove(f, fs.get(), &bad_empty_bin->sect_info), 0);

  UniqueSection bad_size(MakeSection(0, 1000, 150));
  EXPECT_LT(H5FS_sect_remove(f, fs.get(), &bad_size->sect_info), 0);

  UniqueSection bad_addr(MakeSection(0, 9999, 200));
  EXPECT_LT(H5FS_sect_remove(f, fs.get(), &bad_addr->sect_info), 0);

  // 3. Duplicate addr errors in H5FS__sect_link_size & H5FS__sect_link_rest
  //    (same addr, different size -> links into size list, fails merge_list,
  //    rolls back size list via H5FS__sect_unlink_size!).
  UniqueSection dup_same_size(MakeSection(0, 1000, 200));
  EXPECT_LT(
      H5FS_sect_add(f, fs.get(), &dup_same_size->sect_info, 0, &ctx, nullptr),
      0);

  UniqueSection dup_diff_size(MakeSection(0, 1000, 220));
  EXPECT_LT(
      H5FS_sect_add(f, fs.get(), &dup_diff_size->sect_info, 0, &ctx, nullptr),
      0);

  // 4. cls->add failure in H5FS_sect_add and in H5FS_sect_try_extend.
  //    In H5FS_sect_try_extend, `base` is unlinked before cls->add fails, so
  //    free it and re-add a fresh [1000, 200) section for step 5.
  ctx.fail_add = true;
  UniqueSection add_fail(MakeSection(0, 2000, 64));
  EXPECT_LT(H5FS_sect_add(f, fs.get(), &add_fail->sect_info, 0, &ctx, nullptr),
            0);
  EXPECT_LT(H5FS_sect_try_extend(f, fs.get(), 900, 100, 50, 0, &ctx), 0);
  std::free(base);
  ctx.fail_add = false;
  base = MakeSection(0, 1000, 200);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &base->sect_info, 0, &ctx, nullptr), 0);

  // 5. Merge/shrink callback failures in H5FS_sect_add & H5FS_sect_try_merge &
  //    H5FS_sect_try_shrink_eoa:
  // 5a. can_merge fails on less_sect_node:
  ctx.fail_can_merge = true;
  UniqueSection cm_left(MakeSection(0, 1200, 50));
  EXPECT_LT(H5FS_sect_add(f, fs.get(), &cm_left->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  // 5b. can_merge fails on greater_sect_node (addr < base):
  UniqueSection cm_right(MakeSection(0, 900, 50));
  EXPECT_LT(H5FS_sect_add(f, fs.get(), &cm_right->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  ctx.fail_can_merge = false;

  // 5c. merge fails on less_sect_node (note base was unlinked before merge()
  //     is invoked, so restore or free base cleanly afterwards):
  ctx.fail_merge = true;
  UniqueSection m_left(MakeSection(0, 1200, 50));
  EXPECT_LT(H5FS_sect_add(f, fs.get(), &m_left->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  std::free(base);  // base was removed from sinfo before failing merge().
  ctx.fail_merge = false;

  // Re-add base at 1000..1200 and fail merge on greater_sect_node:
  base = MakeSection(0, 1000, 200);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &base->sect_info, 0, &ctx, nullptr), 0);
  ctx.fail_merge = true;
  UniqueSection m_right(MakeSection(0, 900, 100));
  EXPECT_LT(H5FS_sect_add(f, fs.get(), &m_right->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  std::free(base);  // base was removed as greater_sect_node before merge().
  ctx.fail_merge = false;

  // 5d. can_shrink & shrink errors in H5FS_sect_add, try_merge, try_shrink_eoa:
  ctx.fail_can_shrink = true;
  UniqueSection cs_fail(MakeSection(0, 3000, 64));
  EXPECT_LT(H5FS_sect_add(f, fs.get(), &cs_fail->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  EXPECT_LT(H5FS_sect_try_merge(f, fs.get(), &cs_fail->sect_info, 0, &ctx), 0);
  ctx.fail_can_shrink = false;

  ctx.shrink_eoa = 3064;
  ctx.fail_shrink = true;
  UniqueSection sh_fail(MakeSection(0, 3000, 64));
  EXPECT_LT(H5FS_sect_add(f, fs.get(), &sh_fail->sect_info,
                          H5FS_ADD_RETURNED_SPACE, &ctx, nullptr),
            0);
  ctx.fail_shrink = false;

  // H5FS_sect_try_shrink_eoa can_shrink < 0 and shrink < 0:
  TestSection* eoa_s = MakeSection(0, 3000, 64);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &eoa_s->sect_info, 0, &ctx, nullptr), 0);
  ctx.fail_can_shrink = true;
  EXPECT_LT(H5FS_sect_try_shrink_eoa(f, fs.get(), &ctx), 0);
  ctx.fail_can_shrink = false;

  ctx.fail_shrink = true;
  ctx.shrink_eoa = 3064;
  EXPECT_LT(H5FS_sect_try_shrink_eoa(f, fs.get(), &ctx), 0);
  std::free(eoa_s);  // removed before failing shrink().
  ctx.fail_shrink = false;

  // 6. Leave one section in sinfo for the iterate callback error check below,
  //    and test exact-extend cls->free failure via Class 1 (`c_fail_free`).
  TestSection* mergeable_at_5k = MakeSection(0, 5000, 64);
  ASSERT_GE(
      H5FS_sect_add(f, fs.get(), &mergeable_at_5k->sect_info, 0, &ctx, nullptr),
      0);

  TestSection* ff_sect = MakeSection(1, 6000, 32);
  ASSERT_GE(H5FS_sect_add(f, fs.get(), &ff_sect->sect_info, 0, &ctx, nullptr),
            0);
  EXPECT_LT(H5FS_sect_try_extend(f, fs.get(), 5900, 100, 32, 0, &ctx), 0);
  std::free(ff_sect);

  // 7. Iterate error propagation (runs on mergeable_at_5k).
  auto err_iter = [](H5FS_section_info_t* /*sect*/, void* /*ud*/) -> herr_t {
    return -1;
  };
  EXPECT_LT(H5FS_sect_iterate(f, fs.get(), err_iter, nullptr), 0);
}

TEST(H5FSsectionTest, BogusSectAddrLoadFailureAndChangeClassDuplicateMergeAddr) {
  ScopedFile file = CreateInMemoryFile("h5fs_bogus_sinfo.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(nullptr, f);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  ClassBundle bundle;
  const H5FS_create_t cparam = MakeDefaultCreateParams();
  ScopedErrorSilence silence;

  // 1. Separate clean H5FS_t with bogus sect_addr so H5AC_protect fails cleanly
  //    across every H5FS_sect_* entry point.
  {
    ScopedFspace fs(
        f, H5FS_create(f, nullptr, &cparam, 5, bundle.ptrs, nullptr, 0, 0));
    ASSERT_NE(nullptr, fs.get());
    fs.get()->addr = 0x2000;
    fs.get()->sect_addr = 0x7fffffff00ULL;
    fs.get()->sect_size = 64;
    fs.get()->tot_sect_count = 1;
    fs.get()->serial_sect_count = 1;

    UniqueSection dummy(MakeSection(kClsSerialMerge, 1000, 64));
    ASSERT_NE(nullptr, dummy.get());
    auto nop_iter = [](H5FS_section_info_t* /*sect*/, void* /*ud*/) -> herr_t {
      return 0;
    };

    H5FS_section_info_t* out = nullptr;
    EXPECT_LT(H5FS_sect_find(f, fs.get(), 16, &out), 0);
    EXPECT_LT(H5FS_sect_remove(f, fs.get(), &dummy->sect_info), 0);
    EXPECT_LT(H5FS_sect_add(f, fs.get(), &dummy->sect_info, 0, nullptr, nullptr),
              0);
    EXPECT_LT(H5FS_sect_try_extend(f, fs.get(), 900, 100, 10, 0, nullptr), 0);
    EXPECT_LT(H5FS_sect_try_merge(f, fs.get(), &dummy->sect_info, 0, nullptr),
              0);
    EXPECT_LT(H5FS_sect_iterate(f, fs.get(), nop_iter, nullptr), 0);
    EXPECT_LT(
        H5FS_sect_change_class(f, fs.get(), &dummy->sect_info, kClsSerialSym),
        0);
    EXPECT_LT(H5FS_sect_try_shrink_eoa(f, fs.get(), nullptr), 0);

    // Reset synthetic fields so H5FS_close destroys clean header.
    fs.get()->addr = HADDR_UNDEF;
    fs.get()->sect_addr = HADDR_UNDEF;
    fs.get()->sect_size = 0;
    fs.get()->tot_sect_count = 0;
    fs.get()->serial_sect_count = 0;
  }

  // 2. Duplicate addr on merge_list during H5FS_sect_change_class (sep->merge).
  {
    ScopedFspace fs(
        f, H5FS_create(f, nullptr, &cparam, 5, bundle.ptrs, nullptr, 0, 0));
    ASSERT_NE(nullptr, fs.get());
    TestSection* m = MakeSection(kClsSerialMerge, 5000, 64);
    TestSection* g = MakeSection(kClsGhostSep, 5000, 32);
    ASSERT_GE(H5FS_sect_add(f, fs.get(), &m->sect_info, 0, nullptr, nullptr), 0);
    ASSERT_GE(H5FS_sect_add(f, fs.get(), &g->sect_info, 0, nullptr, nullptr), 0);
    EXPECT_LT(
        H5FS_sect_change_class(f, fs.get(), &g->sect_info, kClsSerialMerge), 0);
  }
}

TEST(H5FSsectionTest, VfdAllocHdrAndSinfoAndPublicFileStrategies) {
  // 1. Exercise H5F_FSPACE_STRATEGY_FSM_AGGR and H5F_FSPACE_STRATEGY_PAGE with
  //    persist=true via public dataset creation/deletion/flush/reopen.
  for (const H5F_fspace_strategy_t strategy :
       {H5F_FSPACE_STRATEGY_FSM_AGGR, H5F_FSPACE_STRATEGY_PAGE}) {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), strategy,
                                         /*persist=*/true, /*threshold=*/1),
              0);
    if (strategy == H5F_FSPACE_STRATEGY_PAGE) {
      ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 4096), 0);
    }

    TempFile tmp_file("h5fs_strategy_" +
                      std::to_string(static_cast<int>(strategy)) + ".h5");
    {
      ScopedFile file(
          H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, fcpl.get(), H5P_DEFAULT));
      ASSERT_TRUE(file.valid());

      // Create multiple differently sized datasets and delete alternating ones
      // to populate multiple section sizes and bins in persisted file FSMs.
      for (int i = 0; i < 8; ++i) {
        const hsize_t dim = static_cast<hsize_t>(16 * (i + 1));
        ScopedSpace space(H5Screate_simple(1, &dim, &dim));
        ASSERT_TRUE(space.valid());
        const std::string name = "/d" + std::to_string(i);
        ScopedDataset dset(H5Dcreate2(file.get(), name.c_str(),
                                      H5T_NATIVE_INT32, space.get(),
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
        ASSERT_TRUE(dset.valid());
        std::vector<int32_t> data(dim, i + 1);
        ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                           H5P_DEFAULT, data.data()),
                  0);
      }
      for (int i = 0; i < 8; i += 2) {
        const std::string name = "/d" + std::to_string(i);
        ASSERT_GE(H5Ldelete(file.get(), name.c_str(), H5P_DEFAULT), 0);
      }

      EXPECT_GT(H5Fget_freespace(file.get()), 0);
    }

    ScopedFile reopened(H5Fopen(tmp_file.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ASSERT_TRUE(reopened.valid());
    EXPECT_GT(H5Fget_freespace(reopened.get()), 0);

    // Allocate into persisted free space and flush again on close.
    constexpr hsize_t kNewDim = 24;
    ScopedSpace space(H5Screate_simple(1, &kNewDim, &kNewDim));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(reopened.get(), "/reused", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
  }

  // 2. Direct H5FS_vfd_alloc_hdr_and_section_info_if_needed tests:
  //    no-op when empty/no sinfo, normal allocation, pre-hdr tmp-addr check,
  //    and post-hdr pre-sinfo tmp-addr failure triggering done: cleanup!
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(),
                                         H5F_FSPACE_STRATEGY_FSM_AGGR,
                                         /*persist=*/true, /*threshold=*/1),
              0);
    ScopedFile file =
        CreateInMemoryFile("h5fs_vfd_alloc.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(nullptr, f);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    ClassBundle bundle;
    const H5FS_create_t cparam = MakeDefaultCreateParams();

    // 2a. Empty (serial_sect_count == 0) is a clean no-op.
    {
      ScopedFspace fs(
          f, H5FS_create(f, nullptr, &cparam, 5, bundle.ptrs, nullptr, 0, 0));
      ASSERT_NE(nullptr, fs.get());
      haddr_t out_addr = HADDR_UNDEF;
      EXPECT_GE(H5FS_vfd_alloc_hdr_and_section_info_if_needed(f, fs.get(),
                                                              &out_addr),
                0);
      EXPECT_EQ(HADDR_UNDEF, out_addr);
    }

    // 2b. Floating serializable section triggers header + sinfo allocation.
    {
      H5FS_t* raw_fs =
          H5FS_create(f, nullptr, &cparam, 5, bundle.ptrs, nullptr, 0, 0);
      ASSERT_NE(nullptr, raw_fs);
      TestSection* s = MakeSection(kClsSerialMerge, 1024, 64, 1);
      ASSERT_GE(H5FS_sect_add(f, raw_fs, &s->sect_info, 0, nullptr, nullptr),
                0);
      haddr_t out_addr = HADDR_UNDEF;
      ASSERT_GE(H5FS_vfd_alloc_hdr_and_section_info_if_needed(f, raw_fs,
                                                              &out_addr),
                0);
      EXPECT_NE(HADDR_UNDEF, out_addr);
      EXPECT_EQ(out_addr, raw_fs->addr);
      EXPECT_NE(HADDR_UNDEF, raw_fs->sect_addr);
      EXPECT_EQ(nullptr, raw_fs->sinfo);
      ASSERT_GE(H5FS_close(f, raw_fs), 0);
    }

    // 2c. Pre-header tmp_addr overlap check (line 2369) and post-header
    //     pre-sinfo tmp_addr overlap check (line 2400 -> done: cleanup).
    //     Use separate small Core files with meta_block_size=0 and
    //     H5MF_alloc_tmp so tmp_addr sits at an exact EOA offset.
    {
      ScopedPlist noaggr_fapl = MakeCoreFapl();
      ASSERT_TRUE(noaggr_fapl.valid());
      ASSERT_GE(H5Pset_meta_block_size(noaggr_fapl.get(), 0), 0);

      ScopedFile tmp_file(H5Fcreate("h5fs_vfd_pre_hdr_tmp.h5", H5F_ACC_TRUNC,
                                    fcpl.get(), noaggr_fapl.get()));
      ASSERT_TRUE(tmp_file.valid());
      H5F_t* tf = GetH5File(tmp_file.get());
      ASSERT_NE(nullptr, tf);

      ScopedFspace fs(
          tf, H5FS_create(tf, nullptr, &cparam, 5, bundle.ptrs, nullptr, 0, 0));
      ASSERT_NE(nullptr, fs.get());
      TestSection* s = MakeSection(kClsSerialMerge, 2048, 64, 2);
      ASSERT_GE(H5FS_sect_add(tf, fs.get(), &s->sect_info, 0, nullptr, nullptr),
                0);

      const haddr_t eoa = H5F_get_eoa(tf, H5FD_MEM_FSPACE_HDR);
      ASSERT_NE(HADDR_UNDEF, eoa);
      const haddr_t probe = H5MF_alloc_tmp(tf, 1);
      ASSERT_NE(HADDR_UNDEF, probe);
      const hsize_t shrink_delta = probe - (eoa + fs.get()->sect_size);
      if (shrink_delta > 0) {
        ASSERT_NE(HADDR_UNDEF, H5MF_alloc_tmp(tf, shrink_delta));
      }

      ScopedErrorSilence silence;
      haddr_t out_addr = HADDR_UNDEF;
      EXPECT_LT(H5FS_vfd_alloc_hdr_and_section_info_if_needed(tf, fs.get(),
                                                              &out_addr),
                0);
      ASSERT_GE(H5FS_sect_remove(tf, fs.get(), &s->sect_info), 0);
      std::free(s);
    }

    {
      ScopedPlist noaggr_fapl = MakeCoreFapl();
      ASSERT_TRUE(noaggr_fapl.valid());
      ASSERT_GE(H5Pset_meta_block_size(noaggr_fapl.get(), 0), 0);

      ScopedFile tmp_file(H5Fcreate("h5fs_vfd_post_hdr_tmp.h5", H5F_ACC_TRUNC,
                                    fcpl.get(), noaggr_fapl.get()));
      ASSERT_TRUE(tmp_file.valid());
      H5F_t* tf = GetH5File(tmp_file.get());
      ASSERT_NE(nullptr, tf);

      ScopedFspace fs(
          tf, H5FS_create(tf, nullptr, &cparam, 5, bundle.ptrs, nullptr, 0, 0));
      ASSERT_NE(nullptr, fs.get());
      TestSection* s = MakeSection(kClsSerialMerge, 2048, 64, 3);
      ASSERT_GE(H5FS_sect_add(tf, fs.get(), &s->sect_info, 0, nullptr, nullptr),
                0);

      const haddr_t eoa = H5F_get_eoa(tf, H5FD_MEM_FSPACE_HDR);
      ASSERT_NE(HADDR_UNDEF, eoa);
      const haddr_t probe = H5MF_alloc_tmp(tf, 1);
      ASSERT_NE(HADDR_UNDEF, probe);
      const haddr_t target_tmp =
          eoa + static_cast<haddr_t>(H5FS_HEADER_SIZE(tf)) +
          fs.get()->sect_size;
      ASSERT_GT(probe, target_tmp);
      ASSERT_NE(HADDR_UNDEF, H5MF_alloc_tmp(tf, probe - target_tmp));

      ScopedErrorSilence silence;
      haddr_t out_addr = HADDR_UNDEF;
      EXPECT_LT(H5FS_vfd_alloc_hdr_and_section_info_if_needed(tf, fs.get(),
                                                              &out_addr),
                0);
      EXPECT_EQ(HADDR_UNDEF, fs.get()->addr);
      ASSERT_GE(H5FS_sect_remove(tf, fs.get(), &s->sect_info), 0);
      std::free(s);
    }
  }
}

TEST(H5FSsectionTest, RelinkCollisionsAndPageVfdExhaustion) {
  // 1. H5FS__sect_link collision errors on try_extend, try_merge, and aligned
  //    split-fragment relinking in H5FS__sect_find_node / H5FS_sect_find.
  {
    ScopedFile file = CreateInMemoryFile("h5fs_relink_collisions.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(nullptr, f);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    ClassBundle bundle;
    const H5FS_create_t cparam = MakeDefaultCreateParams();
    ScopedErrorSilence silence;

    {
      ScopedFspace fs(
          f, H5FS_create(f, nullptr, &cparam, 5, bundle.ptrs, nullptr, 0, 0));
      ASSERT_NE(nullptr, fs.get());

      // 1a. try_extend: adjusted (1000, 100) -> (1040, 60) collides with ghost
      //     SEPAR_OBJ section (1040, 60) in size bins.
      TestSection* ghost_ext = MakeSection(kClsGhostSep, 1040, 60);
      TestSection* s_ext = MakeSection(kClsSerialMerge, 1000, 100);
      ASSERT_GE(
          H5FS_sect_add(f, fs.get(), &ghost_ext->sect_info, 0, nullptr, nullptr),
          0);
      ASSERT_GE(
          H5FS_sect_add(f, fs.get(), &s_ext->sect_info, 0, nullptr, nullptr), 0);

      EXPECT_LT(H5FS_sect_try_extend(f, fs.get(), 900, 100, 40, 0, nullptr), 0);
      UniqueSection unlinked_ext(s_ext);

      // 1b. try_merge: incoming (2000, 64) merges (2064, 36) -> (2000, 100),
      //     colliding with ghost SEPAR_OBJ section (2000, 100).
      TestSection* ghost_mrg = MakeSection(kClsGhostSep, 2000, 100);
      TestSection* s_mrg = MakeSection(kClsSerialMerge, 2064, 36);
      ASSERT_GE(
          H5FS_sect_add(f, fs.get(), &ghost_mrg->sect_info, 0, nullptr, nullptr),
          0);
      ASSERT_GE(
          H5FS_sect_add(f, fs.get(), &s_mrg->sect_info, 0, nullptr, nullptr), 0);

      UniqueSection in_mrg(MakeSection(kClsSerialMerge, 2000, 64));
      ASSERT_NE(nullptr, in_mrg.get());
      EXPECT_LT(
          H5FS_sect_try_merge(f, fs.get(), &in_mrg->sect_info, 0, nullptr), 0);
    }

    {
      ScopedFspace fs_align(f, H5FS_create(f, nullptr, &cparam, 5, bundle.ptrs,
                                           nullptr, /*alignment=*/64,
                                           /*threshold=*/32));
      ASSERT_NE(nullptr, fs_align.get());

      // 1c. H5FS__sect_find_node: aligned split of (7200, 80) produces fragment
      //     (7200, 32) colliding with ghost SEPAR_OBJ section (7200, 32).
      TestSection* ghost_frag = MakeSection(kClsGhostSep, 7200, 32);
      TestSection* target = MakeSection(kClsSerialMerge, 7200, 80);
      TestSection* split_frag = nullptr;
      target->split_sink = &split_frag;
      ASSERT_GE(H5FS_sect_add(f, fs_align.get(), &ghost_frag->sect_info, 0,
                              nullptr, nullptr),
                0);
      ASSERT_GE(H5FS_sect_add(f, fs_align.get(), &target->sect_info, 0, nullptr,
                              nullptr),
                0);

      H5FS_section_info_t* found = nullptr;
      EXPECT_LT(H5FS_sect_find(f, fs_align.get(), 40, &found), 0);
      UniqueSection unlinked_found(reinterpret_cast<TestSection*>(found));
      UniqueSection unlinked_frag(split_frag);
    }
  }

  // 2. Paged aggregation H5FS_vfd_alloc_hdr_and_section_info_if_needed VFD
  //    failures (header page exceeds tmp_addr; sinfo second page exceeds
  //    tmp_addr triggering header rollback in done:).
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE,
                                       /*persist=*/true, /*threshold=*/1),
            0);
  ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 4096), 0);

  // 2a. Header allocation (H5FS_HEADER_SIZE > sect_size) hits tmp_addr placed
  //     right past (eoa + sect_size), hitting H5MF_alloc(HDR) failure (line 2380).
  {
    ScopedPlist fcpl_fsm(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl_fsm.valid());
    ASSERT_GE(H5Pset_file_space_strategy(fcpl_fsm.get(),
                                         H5F_FSPACE_STRATEGY_FSM_AGGR,
                                         /*persist=*/true, /*threshold=*/1),
              0);

    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_meta_block_size(fapl.get(), 0), 0);

    ScopedFile file(H5Fcreate("h5fs_vfd_fsm_hdr_fail.h5", H5F_ACC_TRUNC,
                              fcpl_fsm.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* tf = GetH5File(file.get());
    ASSERT_NE(nullptr, tf);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    ClassBundle bundle;
    const H5FS_create_t cparam = MakeDefaultCreateParams();
    ScopedFspace fs(
        tf, H5FS_create(tf, nullptr, &cparam, 5, bundle.ptrs, nullptr, 0, 0));
    ASSERT_NE(nullptr, fs.get());

    TestSection* s = MakeSection(kClsSerialMerge, 1000, 64, 1);
    ASSERT_GE(H5FS_sect_add(tf, fs.get(), &s->sect_info, 0, nullptr, nullptr),
              0);
    ASSERT_LT(fs.get()->sect_size + 1,
              static_cast<hsize_t>(H5FS_HEADER_SIZE(tf)));

    const haddr_t eoa = H5F_get_eoa(tf, H5FD_MEM_FSPACE_HDR);
    ASSERT_NE(HADDR_UNDEF, eoa);
    const haddr_t probe = H5MF_alloc_tmp(tf, 1);
    ASSERT_NE(HADDR_UNDEF, probe);
    const haddr_t target_tmp = eoa + fs.get()->sect_size + 1;
    ASSERT_GT(probe, target_tmp);
    ASSERT_NE(HADDR_UNDEF, H5MF_alloc_tmp(tf, probe - target_tmp));

    ScopedErrorSilence silence;
    haddr_t out_addr = HADDR_UNDEF;
    EXPECT_LT(
        H5FS_vfd_alloc_hdr_and_section_info_if_needed(tf, fs.get(), &out_addr),
        0);
    ASSERT_GE(H5FS_sect_remove(tf, fs.get(), &s->sect_info), 0);
    std::free(s);
  }

  // 2b. Header allocates page 1 (4096), leaving < sect_size free in META;
  //     sinfo (< 4096) misses META and needs page 2, hitting tmp_addr and
  //     rolling back the cached + allocated header.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_meta_block_size(fapl.get(), 0), 0);

    ScopedFile file(H5Fcreate("h5fs_vfd_page_sinfo_fail.h5", H5F_ACC_TRUNC,
                              fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* tf = GetH5File(file.get());
    ASSERT_NE(nullptr, tf);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    ClassBundle bundle(/*c0_serial_size=*/64);
    const H5FS_create_t cparam = MakeDefaultCreateParams();
    ScopedFspace fs(
        tf, H5FS_create(tf, nullptr, &cparam, 5, bundle.ptrs, nullptr, 0, 0));
    ASSERT_NE(nullptr, fs.get());

    const hsize_t hdr_size = static_cast<hsize_t>(H5FS_HEADER_SIZE(tf));
    std::vector<TestSection*> added;
    for (int i = 0; i < 80 && fs.get()->sect_size <= (4096 - hdr_size + 16);
         ++i) {
      TestSection* s = MakeSection(kClsSerialMerge,
                                   static_cast<haddr_t>(10000 + i * 200),
                                   static_cast<hsize_t>(100 + i), i + 1);
      ASSERT_NE(nullptr, s);
      ASSERT_GE(H5FS_sect_add(tf, fs.get(), &s->sect_info, 0, nullptr, nullptr),
                0);
      added.push_back(s);
    }
    ASSERT_GT(fs.get()->sect_size, 4096 - hdr_size);
    ASSERT_LT(fs.get()->sect_size, 4090U);

    const haddr_t eoa = H5F_get_eoa(tf, H5FD_MEM_FSPACE_HDR);
    ASSERT_NE(HADDR_UNDEF, eoa);
    const haddr_t probe = H5MF_alloc_tmp(tf, 1);
    ASSERT_NE(HADDR_UNDEF, probe);
    const haddr_t target_tmp = eoa + fs.get()->sect_size + 1;
    ASSERT_GT(probe, target_tmp);
    ASSERT_NE(HADDR_UNDEF, H5MF_alloc_tmp(tf, probe - target_tmp));

    ScopedErrorSilence silence;
    haddr_t out_addr = HADDR_UNDEF;
    EXPECT_LT(
        H5FS_vfd_alloc_hdr_and_section_info_if_needed(tf, fs.get(), &out_addr),
        0);
    EXPECT_EQ(HADDR_UNDEF, fs.get()->addr);

    for (TestSection* s : added) {
      ASSERT_GE(H5FS_sect_remove(tf, fs.get(), &s->sect_info), 0);
      std::free(s);
    }
  }
}

TEST(H5FSsectionTest, SinfoUnlockXfreeFailureAllCallers) {
  enum class CallerOp {
    kRemove,
    kAdd,
    kTryExtend,
    kTryMerge,
    kFind,
    kIterate,
    kChangeClass,
    kTryShrinkEoa,
  };

  for (const CallerOp op :
       {CallerOp::kRemove, CallerOp::kAdd, CallerOp::kTryExtend,
        CallerOp::kTryMerge, CallerOp::kFind, CallerOp::kIterate,
        CallerOp::kChangeClass, CallerOp::kTryShrinkEoa}) {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_meta_block_size(fapl.get(), 0), 0);

    const std::string fname =
        "h5fs_xfree_fail_" + std::to_string(static_cast<int>(op)) + ".h5";
    ScopedFile file(
        H5Fcreate(fname.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(nullptr, f);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    ClassBundle bundle;
    const H5FS_create_t cparam = MakeDefaultCreateParams();
    haddr_t fs_addr = HADDR_UNDEF;
    ScopedFspace fs(
        f, H5FS_create(f, &fs_addr, &cparam, 5, bundle.ptrs, nullptr, 0, 0));
    ASSERT_NE(nullptr, fs.get());
    ASSERT_NE(HADDR_UNDEF, fs_addr);

    const haddr_t s0_addr = (op == CallerOp::kTryMerge) ? 1064 : 1000;
    const hsize_t s0_size = (op == CallerOp::kTryMerge) ? 36 : 64;
    TestSection* s0 = MakeSection(kClsSerialMerge, s0_addr, s0_size, 1);
    ASSERT_NE(nullptr, s0);
    ASSERT_GE(H5FS_sect_add(f, fs.get(), &s0->sect_info, 0, nullptr, nullptr),
              0);

    // Materialize sinfo in file space + cache, allocate a guard block past it,
    // and pre-insert [sect_addr, alloc_sect_size) into the file's non-
    // persistent FSM so subsequent H5FS__sinfo_unlock H5MF_xfree fails on
    // duplicate address.
    ASSERT_GE(H5FS_alloc_sect(f, fs.get()), 0);
    const haddr_t old_sect_addr = fs.get()->sect_addr;
    const hsize_t old_sect_size = fs.get()->alloc_sect_size;
    ASSERT_NE(HADDR_UNDEF, old_sect_addr);
    ASSERT_GT(old_sect_size, 0U);
    ASSERT_NE(HADDR_UNDEF, H5MF_alloc(f, H5FD_MEM_FSPACE_SINFO, 64));
    ASSERT_GE(
        H5MF_xfree(f, H5FD_MEM_FSPACE_SINFO, old_sect_addr, old_sect_size), 0);

    ScopedErrorSilence silence;
    switch (op) {
      case CallerOp::kRemove: {
        EXPECT_LT(H5FS_sect_remove(f, fs.get(), &s0->sect_info), 0);
        std::free(s0);
        break;
      }
      case CallerOp::kAdd: {
        TestSection* s1 = MakeSection(kClsSerialMerge, 2000, 80, 2);
        ASSERT_NE(nullptr, s1);
        EXPECT_LT(
            H5FS_sect_add(f, fs.get(), &s1->sect_info, 0, nullptr, nullptr), 0);
        EXPECT_GE(H5FS_sect_remove(f, fs.get(), &s0->sect_info), 0);
        EXPECT_GE(H5FS_sect_remove(f, fs.get(), &s1->sect_info), 0);
        std::free(s0);
        std::free(s1);
        break;
      }
      case CallerOp::kTryExtend: {
        TestOpsContext ctx;
        ctx.drop_on_add = true;
        EXPECT_LT(H5FS_sect_try_extend(f, fs.get(), 900, 100, 32, 0, &ctx), 0);
        break;
      }
      case CallerOp::kTryMerge: {
        TestOpsContext ctx;
        ctx.merge_vanish_size = 100;
        TestSection* s1 = MakeSection(kClsSerialMerge, 1000, 64, 2);
        ASSERT_NE(nullptr, s1);
        EXPECT_LT(H5FS_sect_try_merge(f, fs.get(), &s1->sect_info, 0, &ctx), 0);
        break;
      }
      case CallerOp::kFind: {
        H5FS_section_info_t* found = nullptr;
        EXPECT_LT(H5FS_sect_find(f, fs.get(), 64, &found), 0);
        std::free(found);
        break;
      }
      case CallerOp::kIterate: {
        struct IterCtx {
          H5F_t* f;
          H5FS_t* fs;
        } iter_ctx{f, fs.get()};
        auto iter_cb = [](H5FS_section_info_t* sect, void* ud) -> herr_t {
          auto* c = static_cast<IterCtx*>(ud);
          return H5FS_sect_change_class(c->f, c->fs, sect, kClsSerialSym);
        };
        EXPECT_LT(H5FS_sect_iterate(f, fs.get(), iter_cb, &iter_ctx), 0);
        EXPECT_GE(H5FS_sect_remove(f, fs.get(), &s0->sect_info), 0);
        std::free(s0);
        break;
      }
      case CallerOp::kChangeClass: {
        EXPECT_LT(
            H5FS_sect_change_class(f, fs.get(), &s0->sect_info, kClsGhostMerge),
            0);
        break;
      }
      case CallerOp::kTryShrinkEoa: {
        TestOpsContext ctx;
        ctx.shrink_eoa = 1064;
        EXPECT_LT(H5FS_sect_try_shrink_eoa(f, fs.get(), &ctx), 0);
        break;
      }
    }
  }
}

}  // namespace
}  // namespace hdf5
