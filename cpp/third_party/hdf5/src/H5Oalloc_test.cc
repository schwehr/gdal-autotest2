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
// Tests object header chunk and message allocation, null-message selection,
// gap creation/elimination, chunk extension/creation/shrinking, forward
// message packing, continuation flattening, empty-chunk removal, and SWMR flush
// dependency updates (third_party/hdf5/src/H5Oalloc.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Oalloc.c

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match internal struct layouts across C++.
#ifndef NDEBUG
#define H5OALLOC_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5OALLOC_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OALLOC_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

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

ScopedFile CreateCoreFileWithBounds(const char* name, H5F_libver_t low,
                                    H5F_libver_t high,
                                    hid_t fcpl = H5P_DEFAULT) {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_libver_bounds(fapl.get(), low, high) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fcreate(name, H5F_ACC_TRUNC, fcpl, fapl.get()));
}

ScopedPlist MakeCompactAttrOcpl(unsigned max_compact = 128,
                                unsigned min_dense = 64,
                                bool track_times = true) {
  ScopedPlist ocpl(H5Pcreate(H5P_GROUP_CREATE));
  if (!ocpl.valid()) {
    return ScopedPlist();
  }
  if (H5Pset_attr_phase_change(ocpl.get(), max_compact, min_dense) < 0) {
    return ScopedPlist();
  }
  if (H5Pset_obj_track_times(ocpl.get(), track_times) < 0) {
    return ScopedPlist();
  }
  return ocpl;
}

H5F_t* GetInternalFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

void AddBlobAttr(hid_t obj_id, const char* name, size_t nbytes,
                 uint8_t fill = 0x5a) {
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedType opaque(H5Tcreate(H5T_OPAQUE, nbytes));
  ASSERT_TRUE(opaque.valid());
  ScopedAttr attr(H5Acreate2(obj_id, name, opaque.get(), scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  std::vector<uint8_t> data(nbytes, fill);
  EXPECT_GE(H5Awrite(attr.get(), opaque.get(), data.data()), 0);
}

void AddCompactIntAttr(hid_t obj_id, const char* name, int32_t value = 1) {
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedAttr attr(H5Acreate2(obj_id, name, H5T_STD_I32LE, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  EXPECT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, &value), 0);
}

// Removes all attributes whose names appear in `names` on `obj_id`.
void DeleteAttrs(hid_t obj_id, const std::vector<std::string>& names) {
  for (const std::string& name : names) {
    EXPECT_GE(H5Adelete(obj_id, name.c_str()), 0);
  }
}

// Custom raw_size callbacks for exercising H5O__alloc error branches.
size_t ZeroRawSize(const H5F_t* /*f*/, bool /*disable_shared*/,
                   const void* /*mesg*/) {
  return 0;
}

size_t OversizedRawSize(const H5F_t* /*f*/, bool /*disable_shared*/,
                        const void* /*mesg*/) {
  return H5O_MESG_MAX_SIZE;
}

TEST(H5OallocTest, AllocRejectsZeroAndOversizedRawSizeAndDoublesSlots) {
  ScopedFile file = CreateCoreFileWithBounds(
      "alloc_errors.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());
  ScopedPlist ocpl = MakeCompactAttrOcpl();
  ASSERT_TRUE(ocpl.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/g", H5P_DEFAULT, ocpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  H5F_t* f = GetInternalFile(file.get());
  H5O_loc_t* oloc = H5O_get_loc(grp.get());
  ASSERT_NE(f, nullptr);
  ASSERT_NE(oloc, nullptr);

  {
    ScopedApiContext ctx(oloc->addr);
    ASSERT_TRUE(ctx.valid());
    H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    auto unprotect = absl::MakeCleanup([&]() {
      EXPECT_GE(H5O_unprotect(oloc, oh, H5AC__NO_FLAGS_SET), 0);
    });

    H5O_msg_class_t zero_cls = *H5O_MSG_MTIME_NEW;
    zero_cls.raw_size = ZeroRawSize;
    H5O_msg_class_t huge_cls = *H5O_MSG_MTIME_NEW;
    huge_cls.raw_size = OversizedRawSize;

    int dummy = 0;
    size_t idx = 0;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5O__alloc(f, oh, &zero_cls, &dummy, &idx), 0);
      EXPECT_LT(H5O__alloc(f, oh, &huge_cls, &dummy, &idx), 0);
    }

    size_t old_alloc = oh->alloc_nmesgs;
    EXPECT_GE(H5O__alloc_msgs(oh, old_alloc + 5), 0);
    EXPECT_GE(oh->alloc_nmesgs, old_alloc * 2 + 5);
  }

  // Also trigger H5O__alloc_msgs doubling inside natural attribute additions.
  for (int i = 0; i < 16; ++i) {
    AddCompactIntAttr(grp.get(), ("a_" + std::to_string(i)).c_str(), i);
  }
  // Fill remaining slots up to alloc_nmesgs - 1 and add another attr + block
  // chunk 0 so extend/new chunk paths also expand slots cleanly.
  ScopedGroup blocker(
      H5Gcreate2(file.get(), "/blocker", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(blocker.valid());
  for (int i = 16; i < 36; ++i) {
    AddCompactIntAttr(grp.get(), ("a_" + std::to_string(i)).c_str(), i);
  }
}

TEST(H5OallocTest, ExtendChunk0GrowAndShrinkFlagsAndContChunkShrink) {
  // 1) Group with track_times = false so kept metadata is < H5O_MIN_SIZE (22B)
  //    when large attributes are deleted, hitting chunk0 grow (1->2->4 bytes)
  //    and chunk0 shrink back (4->1 byte + H5O_MIN_SIZE null message insert).
  ScopedFile file = CreateCoreFileWithBounds(
      "grow_shrink.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());
  ScopedPlist ocpl_min = MakeCompactAttrOcpl(128, 64, /*track_times=*/false);
  ASSERT_TRUE(ocpl_min.valid());
  ScopedGroup g_min(H5Gcreate2(file.get(), "/g_min", H5P_DEFAULT, ocpl_min.get(),
                               H5P_DEFAULT));
  ASSERT_TRUE(g_min.valid());

  // Grow chunk 0: <=255 -> >255 (2B size) -> >65535 (4B size).
  AddBlobAttr(g_min.get(), "b300", 300);
  AddBlobAttr(g_min.get(), "b35k_a", 35000);
  AddBlobAttr(g_min.get(), "b35k_b", 35000);

  H5O_loc_t* min_loc = H5O_get_loc(g_min.get());
  ASSERT_NE(min_loc, nullptr);
  {
    ScopedApiContext ctx(min_loc->addr);
    H5O_t* oh = H5O_protect(min_loc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    EXPECT_EQ(oh->flags & H5O_HDR_CHUNK0_SIZE, H5O_HDR_CHUNK0_4);
    EXPECT_GE(H5O_unprotect(min_loc, oh, H5AC__NO_FLAGS_SET), 0);
  }

  // Delete all three attributes: merges adjacent 35KB nulls >= 65536 and
  // triggers H5O__alloc_shrink_chunk(chunkno=0) with < min_chunk_size -> 1B.
  DeleteAttrs(g_min.get(), {"b300", "b35k_a", "b35k_b"});
  {
    ScopedApiContext ctx(min_loc->addr);
    H5O_t* oh = H5O_protect(min_loc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    EXPECT_EQ(oh->flags & H5O_HDR_CHUNK0_SIZE, H5O_HDR_CHUNK0_1);
    EXPECT_GE(H5O_unprotect(min_loc, oh, H5AC__NO_FLAGS_SET), 0);
  }

  // 2) Second group keeps a 400-byte attribute after merging two 35KB nulls,
  //    hitting 4B -> 2B chunk0 flag shrink AND sliding messages down past
  //    deleted null messages.
  ScopedPlist ocpl = MakeCompactAttrOcpl(128, 64, /*track_times=*/true);
  ScopedGroup g_mid(
      H5Gcreate2(file.get(), "/g_mid", H5P_DEFAULT, ocpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(g_mid.valid());
  AddBlobAttr(g_mid.get(), "big1", 35000);
  AddBlobAttr(g_mid.get(), "big2", 35000);
  AddBlobAttr(g_mid.get(), "keep400", 400);
  DeleteAttrs(g_mid.get(), {"big1", "big2"});
  H5O_loc_t* mid_loc = H5O_get_loc(g_mid.get());
  {
    ScopedApiContext ctx(mid_loc->addr);
    H5O_t* oh = H5O_protect(mid_loc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    EXPECT_EQ(oh->flags & H5O_HDR_CHUNK0_SIZE, H5O_HDR_CHUNK0_2);
    EXPECT_GE(H5O_unprotect(mid_loc, oh, H5AC__NO_FLAGS_SET), 0);
  }

  // 3) Shrink chunk 0 below min_chunk_size (< 22B kept) AND shrink a
  //    continuation chunk while sliding a kept message down past the shrunk
  //    >= 65536 null:
  // 3a) Committed datatype H5T_STD_I8LE (track_times = false -> ONLY 12B DTYPE)
  //     allocates two 35KB NAME messages directly (no AINFO!) and releases
  //     them -> H5O__alloc_shrink_chunk sees total_msg_size (12) < 22 (lines
  //     2396-2410) and inserts a H5O_MIN_SIZE null message.
  {
    ScopedFile ct_file = CreateCoreFileWithBounds(
        "shrink_min_dt.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(ct_file.valid());
    ScopedPlist tcpl(H5Pcreate(H5P_DATATYPE_CREATE));
    ASSERT_TRUE(tcpl.valid());
    ASSERT_GE(H5Pset_obj_track_times(tcpl.get(), false), 0);
    ScopedType dt(H5Tcopy(H5T_STD_I8LE));
    ASSERT_TRUE(dt.valid());
    ASSERT_GE(H5Tcommit2(ct_file.get(), "/ct_min", dt.get(), H5P_DEFAULT,
                         tcpl.get(), H5P_DEFAULT),
              0);

    H5F_t* cf = GetInternalFile(ct_file.get());
    H5O_loc_t* ct_loc = H5O_get_loc(dt.get());
    ScopedApiContext ctx(ct_loc->addr);
    H5O_t* oh = H5O_protect(ct_loc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    auto alloc_name = [&](size_t raw_sz) -> size_t {
      std::string s(raw_sz - 1, 's');
      H5O_name_t nm = {s.data()};
      size_t idx = 0;
      EXPECT_GE(H5O__alloc(cf, oh, H5O_MSG_NAME, &nm, &idx), 0);
      return idx;
    };
    alloc_name(35000);
    alloc_name(35000);
    for (size_t i = 0; i < oh->nmesgs; ++i) {
      if (oh->mesg[i].type->id == H5O_NAME_ID) {
        EXPECT_GE(H5O__release_mesg(cf, oh, &oh->mesg[i], false), 0);
      }
    }
    EXPECT_GE(H5O__condense_header(cf, oh), 0);
    EXPECT_EQ(oh->flags & H5O_HDR_CHUNK0_SIZE, H5O_HDR_CHUNK0_1);
    EXPECT_GE(H5O_unprotect(ct_loc, oh, H5AC__DIRTIED_FLAG), 0);
  }

  // 3b) Fresh file: pack chunk 0 with zero nulls, block chunk 0, place
  //     [big1(35KB), big2(35KB), keep(100B)] inside chunk 1 (no nulls in chk0 so
  //     keep stays in chk1!), release big1+big2 -> shrink_chunk(chunkno=1)
  //     slides `keep` down (lines 2362-2374) and updates CONT in chunk 0.
  {
    ScopedFile cfile = CreateCoreFileWithBounds(
        "shrink_cont1.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(cfile.valid());
    ScopedGroup g_cont(H5Gcreate2(cfile.get(), "/g_cont", H5P_DEFAULT,
                                  ocpl_min.get(), H5P_DEFAULT));
    ASSERT_TRUE(g_cont.valid());
    H5F_t* cf = GetInternalFile(cfile.get());
    H5O_loc_t* cont_loc = H5O_get_loc(g_cont.get());

    size_t cont_slot = 0;
    {
      ScopedApiContext ctx(cont_loc->addr);
      H5O_t* oh = H5O_protect(cont_loc, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      auto alloc_name = [&](size_t raw_sz) -> size_t {
        std::string s(raw_sz - 1, 'k');
        H5O_name_t nm = {s.data()};
        size_t idx = 0;
        EXPECT_GE(H5O__alloc(cf, oh, H5O_MSG_NAME, &nm, &idx), 0);
        return idx;
      };
      for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < oh->nmesgs; ++i) {
          if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
              oh->mesg[i].raw_size >= 1) {
            alloc_name(oh->mesg[i].raw_size);
            progress = true;
            break;
          }
        }
      }
      cont_slot = alloc_name(16);  // Exact CONT(16B) slot in chunk 0.
      alloc_name(24);
      EXPECT_GE(H5O__release_mesg(cf, oh, &oh->mesg[cont_slot], false), 0);
      EXPECT_GE(H5O_unprotect(cont_loc, oh, H5AC__DIRTIED_FLAG), 0);
    }

    ScopedGroup g_cont_blk(H5Gcreate2(cfile.get(), "/g_cont_blk", H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g_cont_blk.valid());

    ScopedApiContext ctx(cont_loc->addr);
    H5O_t* oh = H5O_protect(cont_loc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    auto alloc_name = [&](size_t raw_sz) -> size_t {
      std::string s(raw_sz - 1, 'm');
      H5O_name_t nm = {s.data()};
      size_t idx = 0;
      EXPECT_GE(H5O__alloc(cf, oh, H5O_MSG_NAME, &nm, &idx), 0);
      return idx;
    };
    size_t big1 = alloc_name(35000);
    size_t big2 = alloc_name(35000);
    size_t keep = alloc_name(100);
    ASSERT_EQ(oh->nchunks, 2u);
    ASSERT_EQ(oh->mesg[keep].chunkno, 1u);
    size_t old_chunk1_size = oh->chunk[1].size;
    EXPECT_GT(old_chunk1_size, 65536u);

    EXPECT_GE(H5O__release_mesg(cf, oh, &oh->mesg[big1], false), 0);
    EXPECT_GE(H5O__release_mesg(cf, oh, &oh->mesg[big2], false), 0);
    EXPECT_GE(H5O__condense_header(cf, oh), 0);

    EXPECT_EQ(oh->nchunks, 2u);
    EXPECT_LT(oh->chunk[1].size, old_chunk1_size);
    EXPECT_LT(oh->chunk[1].size, 1000u);
    EXPECT_GE(H5O__release_mesg(cf, oh, &oh->mesg[keep], false), 0);
    for (size_t i = 0; i < oh->nmesgs; ++i) {
      if (oh->mesg[i].type->id == H5O_NAME_ID) {
        EXPECT_GE(H5O__release_mesg(cf, oh, &oh->mesg[i], false), 0);
      }
    }
    EXPECT_GE(H5O__condense_header(cf, oh), 0);
    EXPECT_GE(H5O_unprotect(cont_loc, oh, H5AC__DIRTIED_FLAG), 0);
  }
}

TEST(H5OallocTest, BestNullAndGapProductionAndElimination) {
  // Craft exact null-message arrangements and chunk gaps via internal H5O
  // messages so every branch of H5O__alloc_find_best_null, H5O__alloc_null,
  // H5O__add_gap, H5O__eliminate_gap, and H5O__release_mesg executes cleanly.
  ScopedFile file = CreateCoreFileWithBounds(
      "gaps_and_nulls.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());
  ScopedPlist ocpl = MakeCompactAttrOcpl(128, 64, /*track_times=*/false);
  ASSERT_TRUE(ocpl.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/g", H5P_DEFAULT, ocpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // Force 3 chunks on /g by blocking after each chunk allocation.
  AddBlobAttr(grp.get(), "c0_anchor", 32);
  ScopedGroup b1(
      H5Gcreate2(file.get(), "/b1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(b1.valid());

  // Now chunk 0 is blocked; add attributes to create chunk 1, block chunk 1,
  // then create chunk 2! Also keep attributes of controlled sizes in each
  // chunk so deleting them leaves nulls in exact tie-breaking positions.
  AddBlobAttr(grp.get(), "c1_pad1", 40);
  AddBlobAttr(grp.get(), "c1_pad2", 40);
  ScopedGroup b2(
      H5Gcreate2(file.get(), "/b2", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(b2.valid());

  AddBlobAttr(grp.get(), "c2_pad1", 40);
  AddBlobAttr(grp.get(), "c2_pad2", 40);

  // Add two attributes at chunk 0 or tail so we have multiple null sizes and
  // create gaps inside v2 chunks directly with H5O_protect + H5O__alloc.
  H5F_t* f = GetInternalFile(file.get());
  H5O_loc_t* oloc = H5O_get_loc(grp.get());
  ASSERT_NE(f, nullptr);
  ASSERT_NE(oloc, nullptr);

  ScopedApiContext ctx(oloc->addr);
  ASSERT_TRUE(ctx.valid());

  // 1) Release c2_pad1 then c1_pad1 via H5O__release_mesg (with oh protected,
  //    not yet condensed!), so oh has equal-sized nulls in chunk 2 before chunk 1.
  //    Then release c0_anchor or create smaller nulls to test tie-breakers.
  H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
  ASSERT_NE(oh, nullptr);
  auto unprotect = absl::MakeCleanup([&]() {
    EXPECT_GE(H5O_unprotect(oloc, oh, H5AC__DIRTIED_FLAG), 0);
  });

  // Helper class using valid H5O_MSG_NAME messages of chosen raw size:
  // H5O_NAME_ID raw_size(s) == strlen(s) + 1.
  auto alloc_name_msg = [&](size_t raw_sz, size_t* out_idx) {
    std::string s(raw_sz - 1, 'x');
    H5O_name_t nmsg = {s.data()};
    size_t idx = 0;
    EXPECT_GE(H5O__alloc(f, oh, H5O_MSG_NAME, &nmsg, &idx), 0);
    if (out_idx != nullptr) {
      *out_idx = idx;
    }
    return idx;
  };

  // Allocate two 20-byte messages followed by a 30-byte message; note they may
  // land in chunk 2 (last chunk), extending it cleanly.
  size_t m_a = alloc_name_msg(20, nullptr);
  size_t m_b = alloc_name_msg(20, nullptr);
  size_t m_c = alloc_name_msg(22, nullptr);
  size_t m_d = alloc_name_msg(22, nullptr);
  size_t m_e = alloc_name_msg(18, nullptr);

  unsigned target_chunk = oh->mesg[m_a].chunkno;
  // Consume any trailing null in target_chunk OR allocate on chunk 0 after
  // freeing all other nulls in chunk 0. Let's isolate nulls in chunk 0:
  // First release all ATTR messages in chunk 2 then chunk 1 then check chunk 0.
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[m_d], false), 0);  // 22B null
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[m_b], false), 0);  // 20B null

  // Allocate an 18B message out of the 20B null (m_b) -> diff == 2 < 4 (msghdr),
  // producing a 2-byte gap while chunk ALSO has another null (m_d) AFTER m_b!
  // Check relative order to hit both null_before_gap == false and true.
  alloc_name_msg(18, nullptr);
  EXPECT_EQ(oh->chunk[target_chunk].gap, 0u);  // Absorbed by m_d!

  // Now eliminate m_d? Wait: m_d was expanded to 22 + 2 = 24B!
  // Let's test gap WITHOUT any other null message in target_chunk:
  // Consume all null messages currently in oh by packing tiny messages or
  // allocate fresh in a clean group with 1 null of known size.
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[m_a], false), 0);  // 20B null
  // Release m_e (18B) too so H5O__alloc_find_best_null sees multiple sizes.
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[m_e], false), 0);  // 18B null
  // Allocate exact 18B fit.
  alloc_name_msg(18, nullptr);
  // Allocate 18B out of 20B null again when m_c is untouched.
  alloc_name_msg(18, nullptr);
  // Condense to leave oh completely clean before unprotecting.
  EXPECT_GE(H5O__condense_header(f, oh), 0);
  (void)m_c;
}

TEST(H5OallocTest, GapWithoutNullAccumulatesToTrailingNullAndEliminates) {
  // Isolate a single chunk object with zero pre-existing nulls so H5O__add_gap
  // takes the `!merged_with_null` path (gap accumulation -> new trailing null,
  // both raw_size == 0 and raw_size > 0) and H5O__eliminate_gap exercises both
  // `null_before_gap == true` and `false` (both with and without middle msgs).
  ScopedFile file = CreateCoreFileWithBounds(
      "gap_isolate.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());
  ScopedPlist ocpl = MakeCompactAttrOcpl(128, 64, /*track_times=*/false);
  ASSERT_TRUE(ocpl.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/g", H5P_DEFAULT, ocpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  H5F_t* f = GetInternalFile(file.get());
  H5O_loc_t* oloc = H5O_get_loc(grp.get());
  ASSERT_NE(f, nullptr);
  ASSERT_NE(oloc, nullptr);

  ScopedApiContext ctx(oloc->addr);
  H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
  ASSERT_NE(oh, nullptr);
  auto unprotect = absl::MakeCleanup([&]() {
    EXPECT_GE(H5O_unprotect(oloc, oh, H5AC__DIRTIED_FLAG), 0);
  });

  auto alloc_name = [&](size_t raw_sz) -> size_t {
    std::string s(raw_sz - 1, 'z');
    H5O_name_t nmsg = {s.data()};
    size_t idx = 0;
    EXPECT_GE(H5O__alloc(f, oh, H5O_MSG_NAME, &nmsg, &idx), 0);
    return idx;
  };

  // Allocate exact messages in chunk 0, and fill any leftover null in chunk 0
  // so ZERO null messages remain in chunk 0.
  for (size_t i = 0; i < oh->nmesgs; ++i) {
    if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0) {
      size_t sz = oh->mesg[i].raw_size;
      if (sz > 0) {
        alloc_name(sz);
      }
      break;
    }
  }
  // Append 3 messages of 10 bytes each and 1 trailing 10-byte message.
  size_t n1 = alloc_name(10);
  size_t n2 = alloc_name(10);
  size_t n3 = alloc_name(10);
  size_t n4 = alloc_name(10);
  size_t n5 = alloc_name(10);

  // Condense/make sure no other null exists in chunk 0 (if chunk 0 had 0B null,
  // remove/consume it? If exact allocation above left 0 null, count == 0!).
  auto clear_other_nulls = [&]() {
    for (size_t i = 0; i < oh->nmesgs; ++i) {
      if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
          oh->mesg[i].raw_size >= 4) {
        alloc_name(oh->mesg[i].raw_size);
      }
    }
  };
  clear_other_nulls();

  // Release n1 (10B) -> allocate 8B -> leaves 2B gap, sliding n2..n5 forward!
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[n1], false), 0);
  if (oh->chunk[0].gap == 0) {
    // Consume any remnant null and produce 2B gap via n2.
    alloc_name(8);
  }
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[n2], false), 0);
  alloc_name(8);
  // Release n3 (10B) when chunk 0 ALREADY has gap > 0: H5O__release_mesg calls
  // H5O__eliminate_gap (line 1358)!
  if (oh->chunk[0].gap == 0) {
    // Produce another 2B gap by extending? Ensure chunk 0 has gap=2 first.
    EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[n3], false), 0);
    alloc_name(8);  // Leaves 2B gap if no other null exists.
  }
  if (oh->chunk[0].gap > 0) {
    // Test H5O__release_mesg eliminating an existing gap.
    EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[n4], false), 0);
  }

  // Test two consecutive gaps accumulating >= msghdr (4B -> 0B raw null; 5B -> 1B raw null).
  clear_other_nulls();
  size_t g1 = alloc_name(10);
  size_t g2 = alloc_name(10);
  size_t g3 = alloc_name(10);
  size_t g4 = alloc_name(10);
  (void)g4;
  // Consume any trailing nulls so gap doesn't absorb early.
  for (size_t i = 0; i < oh->nmesgs; ++i) {
    if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
        oh->mesg[i].raw_size >= 1) {
      alloc_name(oh->mesg[i].raw_size);
    }
  }
  // Produce 2B gap from g1 (diff=2 < 4) then another 2B gap from g2 -> total 4B -> 0B raw null!
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[g1], false), 0);
  alloc_name(8);
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[g2], false), 0);
  alloc_name(8);

  // Produce 3B gap then 2B gap -> total 5B -> 1B raw null!
  size_t g5 = alloc_name(11);
  size_t g6 = alloc_name(10);
  for (size_t i = 0; i < oh->nmesgs; ++i) {
    if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
        oh->mesg[i].raw_size >= 1) {
      alloc_name(oh->mesg[i].raw_size);
    }
  }
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[g5], false), 0);
  alloc_name(8);  // 3B gap
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[g6], false), 0);
  alloc_name(8);  // +2B = 5B -> trailing 1B null

  // Clean up test H5O_NAME messages before closing so cache/condense stay valid.
  for (size_t i = 0; i < oh->nmesgs; ++i) {
    if (oh->mesg[i].type->id == H5O_NAME_ID) {
      EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[i], false), 0);
    }
  }
  EXPECT_GE(H5O__condense_header(f, oh), 0);
  (void)n5;
  (void)g3;
}

TEST(H5OallocTest, BestNullTieBreakAndGapVsNullEliminationCases) {
  // Specifically exercise:
  // 1) H5O__alloc_find_best_null choosing earlier chunk on exact/larger fit
  //    AND early-breaking on chunkno == 0 exact fit.
  // 2) H5O__alloc_null splitting a null when oh->chunk[chunkno].gap > 0 (line 375).
  // 3) H5O__eliminate_gap with null BEFORE gap (adjacent & non-adjacent) and
  //    null AFTER gap (adjacent & non-adjacent).
  ScopedFile file = CreateCoreFileWithBounds(
      "null_tiebreak.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());
  ScopedPlist ocpl = MakeCompactAttrOcpl(128, 64, /*track_times=*/false);
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/g", H5P_DEFAULT, ocpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  H5F_t* f = GetInternalFile(file.get());
  H5O_loc_t* oloc = H5O_get_loc(grp.get());
  ASSERT_NE(f, nullptr);
  ASSERT_NE(oloc, nullptr);

  ScopedApiContext ctx(oloc->addr);
  H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
  ASSERT_NE(oh, nullptr);
  auto unprotect = absl::MakeCleanup([&]() {
    for (size_t i = 0; i < oh->nmesgs; ++i) {
      if (oh->mesg[i].type->id == H5O_NAME_ID) {
        EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[i], false), 0);
      }
    }
    EXPECT_GE(H5O__condense_header(f, oh), 0);
    EXPECT_GE(H5O_unprotect(oloc, oh, H5AC__DIRTIED_FLAG), 0);
  });

  auto alloc_name = [&](size_t raw_sz) -> size_t {
    std::string s(raw_sz - 1, 'k');
    H5O_name_t nmsg = {s.data()};
    size_t idx = 0;
    EXPECT_GE(H5O__alloc(f, oh, H5O_MSG_NAME, &nmsg, &idx), 0);
    return idx;
  };
  auto consume_all_nulls = [&]() {
    for (bool progress = true; progress;) {
      progress = false;
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].raw_size >= 1) {
          alloc_name(oh->mesg[i].raw_size);
          progress = true;
          break;
        }
      }
    }
  };

  // Build 3 deterministic chunks (`chunk 0`, `chunk 1`, `chunk 2`), packing
  // EVERY leftover tail null after each step so no stray null skews selection.
  consume_all_nulls();
  alloc_name(16);  // filler16 in chunk 0 (< 18B LINFO/AINFO)
  size_t c0_big1 = alloc_name(80);
  size_t c0_big2 = alloc_name(80);
  alloc_name(24);  // guard
  consume_all_nulls();
  ScopedGroup blk0(
      H5Gcreate2(file.get(), "/blk0", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(blk0.valid());

  alloc_name(50);  // chunk 1 keep
  alloc_name(16);  // chunk 1 filler16
  size_t c1_big1 = alloc_name(80);
  size_t c1_big2 = alloc_name(80);
  alloc_name(24);  // chunk 1 guard
  consume_all_nulls();
  ScopedGroup blk1(
      H5Gcreate2(file.get(), "/blk1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(blk1.valid());

  size_t c2_20 = alloc_name(20);
  size_t c2_40 = alloc_name(40);
  consume_all_nulls();
  ASSERT_EQ(oh->nchunks, 3u);
  EXPECT_EQ(oh->mesg[c2_20].chunkno, 2u);
  EXPECT_EQ(oh->mesg[c2_40].chunkno, 2u);

  // Split c1_big1 (80B) -> 56B NAME + 20B tail null in chunk 1 (high idx).
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[c1_big1], false), 0);
  alloc_name(80 - 24);
  // Split c0_big1 (80B) -> 56B NAME + 20B tail null in chunk 0 (higher idx).
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[c0_big1], false), 0);
  alloc_name(80 - 24);

  // Release c2_20 (low index in chunk 2): nulls of exact size 20 now sit at
  // [idx(c2_20) chk=2], [chk1_tail chk=1], [chk0_tail chk=0].
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[c2_20], false), 0);
  size_t picked_exact_0 = alloc_name(20);
  EXPECT_EQ(oh->mesg[picked_exact_0].chunkno, 0u);

  // Consume remaining two 20B nulls cleanly so zero nulls exist.
  alloc_name(20);  // consumes chk1_20
  alloc_name(20);  // consumes c2_20

  // Larger fit tie-break + smaller-fit improvement:
  //   c2_40 (low idx, chk2, 40) -> chk0_40 (chk0, 40) -> chk1_24 (chk1, 24).
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[c0_big2], false), 0);
  alloc_name(80 - 44);  // Leaves chk0_40 (40B null in chunk 0).
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[c1_big2], false), 0);
  alloc_name(80 - 28);  // 52B > 40B -> splits c1_big2, leaves chk1_24 (24B).
  EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[c2_40], false), 0);

  size_t picked_better = alloc_name(16);  // Visits chk2(40)->chk0(40)->chk1(24).
  EXPECT_EQ(oh->mesg[picked_better].chunkno, 1u);
}

TEST(H5OallocTest, BestNonnullSelectionAndMultiMessageChunkMove) {
  // 1) Natural multi-message move (`found_msg.msgno < 0` in find_best_nonnull
  //    and H5O__alloc_chunk lines 926-989) using a committed H5T_STD_I8LE
  //    datatype (DTYPE raw_size == 8 < cont_size 16!) + tiny NAME messages +
  //    a small null message in chunk 0 (converting NULL -> H5O_MSG_DELETED!).
  ScopedFile file = CreateCoreFileWithBounds(
      "new_chunk_multi.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());
  ScopedPlist tcpl(H5Pcreate(H5P_DATATYPE_CREATE));
  ASSERT_TRUE(tcpl.valid());
  ASSERT_GE(H5Pset_obj_track_times(tcpl.get(), false), 0);
  ASSERT_GE(H5Pset_attr_phase_change(tcpl.get(), 128, 64), 0);

  ScopedType dt(H5Tcopy(H5T_STD_I8LE));
  ASSERT_TRUE(dt.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/tiny_dt", dt.get(), H5P_DEFAULT,
                       tcpl.get(), H5P_DEFAULT),
            0);

  H5F_t* f = GetInternalFile(file.get());
  H5O_loc_t* oloc = H5O_get_loc(dt.get());
  ASSERT_NE(f, nullptr);
  ASSERT_NE(oloc, nullptr);

  {
    ScopedApiContext ctx(oloc->addr);
    H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    auto unprotect = absl::MakeCleanup([&]() {
      EXPECT_GE(H5O_unprotect(oloc, oh, H5AC__DIRTIED_FLAG), 0);
    });

    auto alloc_name = [&](size_t sz) -> size_t {
      std::string s(sz - 1, 't');
      H5O_name_t nmsg = {s.data()};
      size_t idx = 0;
      EXPECT_GE(H5O__alloc(f, oh, H5O_MSG_NAME, &nmsg, &idx), 0);
      return idx;
    };

    // Consume all initial nulls down to 0, then add tiny 8B NAME + 6B NAME.
    for (bool progress = true; progress;) {
      progress = false;
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
            oh->mesg[i].raw_size >= 1) {
          size_t step = (oh->mesg[i].raw_size > 10) ? 8 : oh->mesg[i].raw_size;
          alloc_name(step);
          progress = true;
          break;
        }
      }
    }
    alloc_name(8);
    size_t n_tiny_del = alloc_name(2);  // 2B null (+4 = 6B total < 16B cont).
    alloc_name(6);

    // Block chunk 0 before releasing n_tiny_del or allocating big message.
    ScopedGroup blocker(
        H5Gcreate2(file.get(), "/blk", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(blocker.valid());

    // Release n_tiny_del without condensing -> 2B NULL sits in chunk 0 before
    // `6B` message (`DTYPE` <= 12B, `6B + 6B = 12B < 16B`). Now EVERY message in
    // chunk 0 has total_size < cont_size (16B)!
    EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[n_tiny_del], false), 0);

    size_t big_idx = alloc_name(50);
    EXPECT_GT(oh->nchunks, 1u);
    EXPECT_EQ(oh->mesg[big_idx].chunkno, 1u);
    EXPECT_GT(oh->num_deleted_mesgs, 0u);

    // Strips H5O_MSG_DELETED entries (lines 2267-2274) and cleans up.
    EXPECT_GE(H5O__condense_header(f, oh), 0);
    for (size_t i = 0; i < oh->nmesgs; ++i) {
      if (oh->mesg[i].type->id == H5O_NAME_ID) {
        EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[i], false), 0);
      }
    }
    EXPECT_GE(H5O__condense_header(f, oh), 0);
  }

  // 2) Single best non-null prioritization (`attr` visited BEFORE `non-attr` ->
  //    hits line 775!): allocate `slot1(80)` BEFORE creating AINFO, add dummy
  //    attr (creating AINFO after slot1), release slot1, then add `a_first`
  //    into slot1 so `ATTR("a_first")` precedes AINFO(18) in oh->mesg[].
  {
    ScopedType dt2(H5Tcopy(H5T_STD_I8LE));
    ASSERT_TRUE(dt2.valid());
    ASSERT_GE(
        H5Tcommit_anon(file.get(), dt2.get(), tcpl.get(), H5P_DEFAULT), 0);

    H5O_loc_t* dloc2 = H5O_get_loc(dt2.get());
    auto pack_tiny_nulls = [&](H5O_t* target_oh, int max_chunk = -1) {
      for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < target_oh->nmesgs; ++i) {
          if (target_oh->mesg[i].type->id == H5O_NULL_ID &&
              (max_chunk < 0 ||
               target_oh->mesg[i].chunkno == static_cast<unsigned>(max_chunk)) &&
              target_oh->mesg[i].raw_size >= 1) {
            size_t step = (target_oh->mesg[i].raw_size > 14)
                              ? 10
                              : target_oh->mesg[i].raw_size;
            std::string fill_s(step - 1, 'f');
            H5O_name_t fmsg = {fill_s.data()};
            size_t fidx = 0;
            EXPECT_GE(H5O__alloc(f, target_oh, H5O_MSG_NAME, &fmsg, &fidx), 0);
            progress = true;
            break;
          }
        }
      }
    };

    size_t slot1 = 0;
    {
      ScopedApiContext ctx(dloc2->addr);
      H5O_t* oh = H5O_protect(dloc2, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      pack_tiny_nulls(oh, 0);
      std::string nm80(79, 's');
      H5O_name_t s80 = {nm80.data()};
      EXPECT_GE(H5O__alloc(f, oh, H5O_MSG_NAME, &s80, &slot1), 0);
      EXPECT_GE(H5O_unprotect(dloc2, oh, H5AC__DIRTIED_FLAG), 0);
    }

    // Adding a small dummy attr creates AINFO AFTER `slot1`.
    AddBlobAttr(dt2.get(), "a_dummy", 12);
    {
      ScopedApiContext ctx(dloc2->addr);
      H5O_t* oh = H5O_protect(dloc2, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[slot1], false), 0);
      EXPECT_GE(H5O_unprotect(dloc2, oh, H5AC__DIRTIED_FLAG), 0);
    }
    // `a_first` (60B) consumes `slot1` (`80B`), leaving `ATTR` strictly before AINFO!
    AddBlobAttr(dt2.get(), "a_first", 32);

    ASSERT_GE(H5Olink(dt2.get(), file.get(), "/prioritize_dt", H5P_DEFAULT,
                      H5P_DEFAULT),
              0);
    ScopedGroup blk_a(H5Gcreate2(file.get(), "/blk_dt2_a", H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(blk_a.valid());

    ScopedApiContext ctx(dloc2->addr);
    H5O_t* oh = H5O_protect(dloc2, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    auto unprotect = absl::MakeCleanup([&]() {
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NAME_ID) {
          EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[i], false), 0);
        }
      }
      EXPECT_GE(H5O__condense_header(f, oh), 0);
      EXPECT_GE(H5O_unprotect(dloc2, oh, H5AC__DIRTIED_FLAG), 0);
    });
    pack_tiny_nulls(oh, 0);

    std::string nm50(49, 'r');
    H5O_name_t p50 = {nm50.data()};
    size_t idx50 = 0;
    EXPECT_GE(H5O__alloc(f, oh, H5O_MSG_NAME, &p50, &idx50), 0);
    EXPECT_EQ(oh->nchunks, 2u);
  }

  // 2b) Single best non-null at END of chunk 0 absorbing `gap_size > 0`
  //     (H5O__alloc_chunk lines 1025-1027). Use an isolated in-memory file with
  //     H5Tcommit_anon so root-group allocations never block chunk 0 early.
  {
    ScopedFile mf = CreateCoreFileWithBounds(
        "anon_gap.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(mf.valid());
    H5F_t* mff = GetInternalFile(mf.get());
    ASSERT_NE(mff, nullptr);
    ScopedType dt_gap(H5Tcopy(H5T_STD_I8LE));
    ASSERT_TRUE(dt_gap.valid());
    ASSERT_GE(
        H5Tcommit_anon(mf.get(), dt_gap.get(), tcpl.get(), H5P_DEFAULT), 0);
    H5O_loc_t* dloc = H5O_get_loc(dt_gap.get());
    {
      ScopedApiContext ctx(dloc->addr);
      H5O_t* oh = H5O_protect(dloc, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      auto alloc_name = [&](size_t sz) -> size_t {
        std::string s(sz - 1, 'g');
        H5O_name_t nm = {s.data()};
        size_t idx = 0;
        EXPECT_GE(H5O__alloc(mff, oh, H5O_MSG_NAME, &nm, &idx), 0);
        return idx;
      };
      for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < oh->nmesgs; ++i) {
          if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
              oh->mesg[i].raw_size >= 1) {
            size_t step = (oh->mesg[i].raw_size > 15) ? 10 : oh->mesg[i].raw_size;
            alloc_name(step);
            progress = true;
            break;
          }
        }
      }
      alloc_name(20);
      size_t m16 = alloc_name(16);
      EXPECT_GE(H5O__release_mesg(mff, oh, &oh->mesg[m16], false), 0);
      alloc_name(14);
      ASSERT_EQ(oh->nchunks, 1u);
      ASSERT_EQ(oh->chunk[0].gap, 2u);
      EXPECT_GE(H5O_unprotect(dloc, oh, H5AC__DIRTIED_FLAG), 0);
    }
    ScopedType blk_gap(H5Tcopy(H5T_STD_I8LE));
    ASSERT_TRUE(blk_gap.valid());
    ASSERT_GE(
        H5Tcommit_anon(mf.get(), blk_gap.get(), H5P_DEFAULT, H5P_DEFAULT), 0);
    ScopedApiContext ctx(dloc->addr);
    H5O_t* oh = H5O_protect(dloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    auto unprotect = absl::MakeCleanup([&]() {
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NAME_ID) {
          EXPECT_GE(H5O__release_mesg(mff, oh, &oh->mesg[i], false), 0);
        }
      }
      EXPECT_GE(H5O__condense_header(mff, oh), 0);
      EXPECT_GE(H5O_unprotect(dloc, oh, H5AC__DIRTIED_FLAG), 0);
    });
    std::string s50(49, 'q');
    H5O_name_t p50 = {s50.data()};
    size_t idx50 = 0;
    EXPECT_GE(H5O__alloc(mff, oh, H5O_MSG_NAME, &p50, &idx50), 0);
    EXPECT_EQ(oh->nchunks, 2u);
    EXPECT_EQ(oh->chunk[0].gap, 0u);
  }

  // 2c) Non-last 32B message moved from chunk 0 while chunk 0 ALSO holds
  //     `gap == 2`: splitting NULL(32) for CONT(16) hits H5O__alloc_null gap-
  //     eliminate lines 376-384 right inside H5O__alloc_chunk!
  {
    ScopedFile mf = CreateCoreFileWithBounds(
        "anon_gn.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(mf.valid());
    H5F_t* mff = GetInternalFile(mf.get());
    ASSERT_NE(mff, nullptr);
    ScopedType dt_gn(H5Tcopy(H5T_STD_I8LE));
    ASSERT_TRUE(dt_gn.valid());
    ASSERT_GE(
        H5Tcommit_anon(mf.get(), dt_gn.get(), tcpl.get(), H5P_DEFAULT), 0);
    H5O_loc_t* dloc = H5O_get_loc(dt_gn.get());
    {
      ScopedApiContext ctx(dloc->addr);
      H5O_t* oh = H5O_protect(dloc, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      auto alloc_name = [&](size_t sz) -> size_t {
        std::string s(sz - 1, 'h');
        H5O_name_t nm = {s.data()};
        size_t idx = 0;
        EXPECT_GE(H5O__alloc(mff, oh, H5O_MSG_NAME, &nm, &idx), 0);
        return idx;
      };
      for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < oh->nmesgs; ++i) {
          if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
              oh->mesg[i].raw_size >= 1) {
            size_t step = (oh->mesg[i].raw_size > 15) ? 10 : oh->mesg[i].raw_size;
            alloc_name(step);
            progress = true;
            break;
          }
        }
      }
      alloc_name(32);  // m32_mid (NOT last -> gap_size == 0; leaves 12B tail null)
      size_t m10_end = alloc_name(10);
      EXPECT_GE(H5O__release_mesg(mff, oh, &oh->mesg[m10_end], false), 0);
      alloc_name(8);  // Produces 2B gap at end (`total_size == 10 < 16`).
      ASSERT_EQ(oh->nchunks, 1u);
      ASSERT_EQ(oh->chunk[0].gap, 2u);
      EXPECT_GE(H5O_unprotect(dloc, oh, H5AC__DIRTIED_FLAG), 0);
    }
    ScopedType blk_gn(H5Tcopy(H5T_STD_I8LE));
    ASSERT_TRUE(blk_gn.valid());
    ASSERT_GE(
        H5Tcommit_anon(mf.get(), blk_gn.get(), H5P_DEFAULT, H5P_DEFAULT), 0);
    ScopedApiContext ctx(dloc->addr);
    H5O_t* oh = H5O_protect(dloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    auto unprotect = absl::MakeCleanup([&]() {
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NAME_ID) {
          EXPECT_GE(H5O__release_mesg(mff, oh, &oh->mesg[i], false), 0);
        }
      }
      EXPECT_GE(H5O__condense_header(mff, oh), 0);
      EXPECT_GE(H5O_unprotect(dloc, oh, H5AC__DIRTIED_FLAG), 0);
    });
    std::string s50(49, 'r');
    H5O_name_t p50 = {s50.data()};
    size_t idx50 = 0;
    EXPECT_GE(H5O__alloc(mff, oh, H5O_MSG_NAME, &p50, &idx50), 0);
    EXPECT_EQ(oh->nchunks, 2u);
    EXPECT_EQ(oh->chunk[0].gap, 0u);
  }

  // 2d) Single best non-null absorbing adjacent trailing NULL (`null_size > 0`
  //     -> H5O__alloc_chunk lines 1029-1044) using committed datatype (no AINFO/LINFO!).
  {
    ScopedFile mf = CreateCoreFileWithBounds(
        "anon_null.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(mf.valid());
    H5F_t* mff = GetInternalFile(mf.get());
    ASSERT_NE(mff, nullptr);
    ScopedType dt_null(H5Tcopy(H5T_STD_I8LE));
    ASSERT_TRUE(dt_null.valid());
    ASSERT_GE(
        H5Tcommit_anon(mf.get(), dt_null.get(), tcpl.get(), H5P_DEFAULT), 0);
    H5O_loc_t* dloc = H5O_get_loc(dt_null.get());
    size_t mid8 = 0;
    {
      ScopedApiContext ctx(dloc->addr);
      H5O_t* oh = H5O_protect(dloc, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      auto alloc_name = [&](size_t sz) -> size_t {
        std::string s(sz - 1, 'n');
        H5O_name_t nm = {s.data()};
        size_t idx = 0;
        EXPECT_GE(H5O__alloc(mff, oh, H5O_MSG_NAME, &nm, &idx), 0);
        return idx;
      };
      for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < oh->nmesgs; ++i) {
          if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
              oh->mesg[i].raw_size >= 1) {
            size_t step = (oh->mesg[i].raw_size > 15) ? 10 : oh->mesg[i].raw_size;
            alloc_name(step);
            progress = true;
            break;
          }
        }
      }
      alloc_name(6);  // Guard so pack_nulls never sits right before mid8.
      alloc_name(8);  // m8_a: raw_size=8 + mid8(4+8=12) => total_size=20 >= 16!
      mid8 = alloc_name(8);
      alloc_name(6);  // Guard after mid8 so mid8 isn't at chunk end.
      ASSERT_EQ(oh->nchunks, 1u);
      EXPECT_GE(H5O_unprotect(dloc, oh, H5AC__DIRTIED_FLAG), 0);
    }
    ScopedType blk_null(H5Tcopy(H5T_STD_I8LE));
    ASSERT_TRUE(blk_null.valid());
    ASSERT_GE(
        H5Tcommit_anon(mf.get(), blk_null.get(), H5P_DEFAULT, H5P_DEFAULT), 0);
    ScopedApiContext ctx(dloc->addr);
    H5O_t* oh = H5O_protect(dloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    auto unprotect = absl::MakeCleanup([&]() {
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NAME_ID) {
          EXPECT_GE(H5O__release_mesg(mff, oh, &oh->mesg[i], false), 0);
        }
      }
      EXPECT_GE(H5O__condense_header(mff, oh), 0);
      EXPECT_GE(H5O_unprotect(dloc, oh, H5AC__DIRTIED_FLAG), 0);
    });
    EXPECT_GE(H5O__release_mesg(mff, oh, &oh->mesg[mid8], false), 0);
    std::string s24(23, 'p');
    H5O_name_t p24 = {s24.data()};
    size_t idx24 = 0;
    EXPECT_GE(H5O__alloc(mff, oh, H5O_MSG_NAME, &p24, &idx24), 0);
    EXPECT_GT(oh->num_deleted_mesgs, 0u);
  }

  // 3) Single best non-null absorbing an adjacent trailing NULL (`null_size > 0`
  //    -> hits lines 1029-1044) + >= 4 chunks to double alloc_nchunks.
  ScopedPlist ocpl = MakeCompactAttrOcpl(128, 64, /*track_times=*/false);
  ScopedGroup g2(
      H5Gcreate2(file.get(), "/best_nonnull", H5P_DEFAULT, ocpl.get(),
                 H5P_DEFAULT));
  ASSERT_TRUE(g2.valid());
  H5O_loc_t* loc2 = H5O_get_loc(g2.get());
  ASSERT_NE(loc2, nullptr);
  {
    ScopedApiContext ctx(loc2->addr);
    H5O_t* oh = H5O_protect(loc2, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    auto unprotect = absl::MakeCleanup([&]() {
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NAME_ID) {
          EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[i], false), 0);
        }
      }
      EXPECT_GE(H5O__condense_header(f, oh), 0);
      EXPECT_GE(H5O_unprotect(loc2, oh, H5AC__DIRTIED_FLAG), 0);
    });

    auto alloc_name = [&](size_t sz) -> size_t {
      std::string s(sz - 1, 'z');
      H5O_name_t nm = {s.data()};
      size_t idx = 0;
      EXPECT_GE(H5O__alloc(f, oh, H5O_MSG_NAME, &nm, &idx), 0);
      return idx;
    };
    for (bool progress = true; progress;) {
      progress = false;
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
            oh->mesg[i].raw_size >= 1) {
          alloc_name(oh->mesg[i].raw_size);
          progress = true;
          break;
        }
      }
    }
    // Allocate [16B, 8B, 8B] sequentially, release middle 8B -> leaves 16B
    // immediately followed by 8B NULL (total 16 + 12 = 28B).
    alloc_name(16);
    size_t mid8 = alloc_name(8);
    alloc_name(8);
    EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[mid8], false), 0);

    ScopedGroup blk3(
        H5Gcreate2(file.get(), "/blk3", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(blk3.valid());

    // Request 24B (> 8B null, cannot fit in chunk 0): moves 16B + absorbs
    // trailing 8B null (`old_null_msg` -> H5O_MSG_DELETED, lines 1029-1044),
    // then add more chunks up to >= 4 chunks.
    alloc_name(24);
    for (int c = 0; c < 4; ++c) {
      alloc_name(64);
      ScopedGroup b(H5Gcreate2(
          file.get(), ("/blk_loop_" + std::to_string(c)).c_str(),
          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(b.valid());
    }
    EXPECT_GE(oh->nchunks, 4u);
    EXPECT_GE(oh->alloc_nchunks, 4u);
  }
}

TEST(H5OallocTest, CondenseMoveContMoveForwardAndEmptyChunkRemoval) {
  // Persist a 4-chunk object where CONT->2 (in chunk 0) and CONT->3 (in chunk 1)
  // sit across chunk boundaries so reopening decodes both with chunkno == 0,
  // and deleting chunk 2's sole attr triggers H5O__remove_empty_chunks on a
  // middle chunk with uninitialized cont->chunkno == 0 AND shifts chunk 3 -> 2
  // (updating cont->chunkno AND H5O__chunk_update_idx!).
  TempFile tmp("condense_reopen.h5");
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18,
                                   H5F_LIBVER_LATEST),
              0);
    ScopedFile file(
        H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    ScopedPlist ocpl = MakeCompactAttrOcpl(128, 64, /*track_times=*/false);
    ScopedGroup grp(
        H5Gcreate2(file.get(), "/g", H5P_DEFAULT, ocpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    H5F_t* f = GetInternalFile(file.get());
    H5O_loc_t* oloc = H5O_get_loc(grp.get());
    {
      ScopedApiContext ctx(oloc->addr);
      H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      auto unprotect = absl::MakeCleanup([&]() {
        EXPECT_GE(H5O_unprotect(oloc, oh, H5AC__DIRTIED_FLAG), 0);
      });
      auto alloc_name = [&](size_t raw_sz) -> size_t {
        std::string s(raw_sz - 1, 'r');
        H5O_name_t nm = {s.data()};
        size_t idx = 0;
        EXPECT_GE(H5O__alloc(f, oh, H5O_MSG_NAME, &nm, &idx), 0);
        return idx;
      };
      for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < oh->nmesgs; ++i) {
          if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
              oh->mesg[i].raw_size >= 1) {
            alloc_name(oh->mesg[i].raw_size);
            progress = true;
            break;
          }
        }
      }
      alloc_name(16);  // chunk 0 filler16 (< 18B LINFO).
      alloc_name(24);  // chunk 0 guard.
      ScopedGroup b0(H5Gcreate2(file.get(), "/b0", H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
      ASSERT_TRUE(b0.valid());

      // Chunk 1: c1_keep (50B) + c1_filler16 (16B) + c1_guard (24B) + c1_hole (20B).
      alloc_name(50);
      alloc_name(16);
      alloc_name(24);
      size_t c1_hole = alloc_name(20);
      ScopedGroup b1(H5Gcreate2(file.get(), "/b1", H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
      ASSERT_TRUE(b1.valid());

      // Chunk 2: c2_filler16 + c2_del (24B) + c2_keep (36B).
      alloc_name(36);
      alloc_name(24);
      ScopedGroup b2(H5Gcreate2(file.get(), "/b2", H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
      ASSERT_TRUE(b2.valid());

      // Chunk 3: c3_keep (50B).
      alloc_name(50);
      ASSERT_EQ(oh->nchunks, 4u);

      // Move CONT->3 into chunk 1's c1_hole (20B) cleanly, and release
      // chunk 2's other msg so chunk 2 holds ONLY ONE attr added below? Actually
      // just keep `c2_del` (`NAME(24)`) in chunk 2 after moving CONT->3 into c1_hole!
      EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[c1_hole], false), 0);
      EXPECT_GE(H5O__condense_header(f, oh), 0);
      EXPECT_EQ(oh->nchunks, 4u);
    }
    // Add one attr into chunk 2? Wait: on reopen, releasing chunk 2's remaining
    // NAME(36)+NAME(24) empties chunk 2! Better: release NAME(36)+NAME(24) after
    // adding attribute? No, H5O__release_mesg works on NAME messages too!
  }

  // Reopen fresh: both CONT->2 and CONT->3 start with cont->chunkno == 0!
  {
    ScopedFile file(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(H5Gopen2(file.get(), "/g", H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    H5F_t* f = GetInternalFile(file.get());
    H5O_loc_t* oloc = H5O_get_loc(grp.get());
    ScopedApiContext ctx(oloc->addr);
    H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    auto unprotect = absl::MakeCleanup([&]() {
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NAME_ID) {
          EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[i], false), 0);
        }
      }
      EXPECT_GE(H5O__condense_header(f, oh), 0);
      EXPECT_GE(H5O_unprotect(oloc, oh, H5AC__DIRTIED_FLAG), 0);
    });
    ASSERT_EQ(oh->nchunks, 4u);
    // Bring chunk 3 proxy into metadata cache (IN_CACHE) so deleting middle
    // chunk 2 shifts chunk 3 -> 2 and invokes H5O__chunk_update_idx!
    H5O_chunk_proxy_t* chk3 = H5O__chunk_protect(f, oh, 3);
    ASSERT_NE(chk3, nullptr);
    EXPECT_GE(H5O__chunk_unprotect(f, chk3, false), 0);

    // Reset cont->chunkno = 0 on decoded H5O_CONT_ID messages to exercise the
    // unresolved-chunkno lookup loops in H5O__remove_empty_chunks (lines
    // 2110-2119 and 2207-2216), then release chunk 2 and condense.
    for (size_t i = 0; i < oh->nmesgs; ++i) {
      if (oh->mesg[i].type->id == H5O_CONT_ID && oh->mesg[i].native != nullptr) {
        static_cast<H5O_cont_t*>(oh->mesg[i].native)->chunkno = 0;
      }
      if (oh->mesg[i].chunkno == 2 && oh->mesg[i].type->id != H5O_NULL_ID) {
        EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[i], false), 0);
      }
    }
    EXPECT_GE(H5O__condense_header(f, oh), 0);
    EXPECT_EQ(oh->nchunks, 3u);
  }

  // Also cover line 2301 (H5O__remove_empty_chunks returning > 0 inside
  // H5O__condense_header when chunk 1 holds a single full-chunk NULL message
  // and neither move_msgs_forward nor merge_null did anything).
  {
    ScopedFile rfile = CreateCoreFileWithBounds(
        "remove_empty_direct.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(rfile.valid());
    ScopedPlist tcpl(H5Pcreate(H5P_DATATYPE_CREATE));
    ASSERT_TRUE(tcpl.valid());
    ASSERT_GE(H5Pset_obj_track_times(tcpl.get(), false), 0);
    ScopedType dt(H5Tcopy(H5T_STD_I8LE));
    ASSERT_TRUE(dt.valid());
    ASSERT_GE(H5Tcommit2(rfile.get(), "/dt_rem", dt.get(), H5P_DEFAULT,
                         tcpl.get(), H5P_DEFAULT),
              0);
    H5F_t* rf = GetInternalFile(rfile.get());
    H5O_loc_t* rloc = H5O_get_loc(dt.get());
    {
      ScopedApiContext ctx(rloc->addr);
      H5O_t* oh = H5O_protect(rloc, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      auto alloc_name = [&](size_t sz) -> size_t {
        std::string s(sz - 1, 'v');
        H5O_name_t nm = {s.data()};
        size_t idx = 0;
        EXPECT_GE(H5O__alloc(rf, oh, H5O_MSG_NAME, &nm, &idx), 0);
        return idx;
      };
      for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < oh->nmesgs; ++i) {
          if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
              oh->mesg[i].raw_size >= 1) {
            alloc_name(oh->mesg[i].raw_size);
            progress = true;
            break;
          }
        }
      }
      size_t c0_cont = alloc_name(16);
      alloc_name(24);
      EXPECT_GE(H5O__release_mesg(rf, oh, &oh->mesg[c0_cont], false), 0);
      EXPECT_GE(H5O_unprotect(rloc, oh, H5AC__DIRTIED_FLAG), 0);
    }
    ScopedGroup blk_r(H5Gcreate2(rfile.get(), "/blk_r", H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(blk_r.valid());
    ScopedApiContext ctx(rloc->addr);
    H5O_t* oh = H5O_protect(rloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    std::string s40(39, 'y');
    H5O_name_t nm40 = {s40.data()};
    size_t c1_only = 0;
    EXPECT_GE(H5O__alloc(rf, oh, H5O_MSG_NAME, &nm40, &c1_only), 0);
    ASSERT_EQ(oh->nchunks, 2u);
    ASSERT_EQ(oh->mesg[c1_only].chunkno, 1u);
    EXPECT_GE(H5O__release_mesg(rf, oh, &oh->mesg[c1_only], false), 0);
    EXPECT_GE(H5O__condense_header(rf, oh), 0);
    EXPECT_EQ(oh->nchunks, 1u);
    for (size_t i = 0; i < oh->nmesgs; ++i) {
      if (oh->mesg[i].type->id == H5O_NAME_ID) {
        EXPECT_GE(H5O__release_mesg(rf, oh, &oh->mesg[i], false), 0);
      }
    }
    EXPECT_GE(H5O__condense_header(rf, oh), 0);
    EXPECT_GE(H5O_unprotect(rloc, oh, H5AC__DIRTIED_FLAG), 0);
  }

  // Also test H5O__move_cont and H5O__move_msgs_forward exact swap + remainder
  // branches in-memory:
  // While chunk 0 is still unblocked, put a 16-byte NAME message (`filler16`)
  // followed by an 8-byte NAME message (`post8`) and consume any remaining
  // chunk 0 nulls so `filler16` has total_size == 16 (< LINFO/AINFO's 18B!).
  // Then block chunk 0 and allocate `small_raw_sz` (12, 14, or 16):
  // `H5O__alloc_find_best_nonnull` picks `filler16` (16B), moves it to chunk 1,
  // places `CONT` (16B raw + 4B hdr = 20B) in chunk 0, and puts `target_idx` in
  // chunk 1. Releasing `filler16` and calling `H5O__condense_header` invokes
  // `H5O__move_cont` leaving: >=4B null (12), 2B gap (14), or exact 0B (16).
  auto run_move_cont_case = [](size_t small_raw_sz, const char* tag) {
    ScopedFile mem = CreateCoreFileWithBounds(
        tag, H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(mem.valid());
    ScopedPlist ocpl = MakeCompactAttrOcpl(128, 64, /*track_times=*/false);
    ScopedGroup g(
        H5Gcreate2(mem.get(), "/g", H5P_DEFAULT, ocpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(g.valid());

    H5F_t* mf = GetInternalFile(mem.get());
    H5O_loc_t* mloc = H5O_get_loc(g.get());
    ScopedApiContext ctx(mloc->addr);
    H5O_t* oh = H5O_protect(mloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    auto unprotect = absl::MakeCleanup([&]() {
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NAME_ID) {
          EXPECT_GE(H5O__release_mesg(mf, oh, &oh->mesg[i], false), 0);
        }
      }
      EXPECT_GE(H5O__condense_header(mf, oh), 0);
      EXPECT_GE(H5O_unprotect(mloc, oh, H5AC__DIRTIED_FLAG), 0);
    });

    auto alloc_name = [&](size_t raw_sz) -> size_t {
      std::string s(raw_sz - 1, 's');
      H5O_name_t nm = {s.data()};
      size_t idx = 0;
      EXPECT_GE(H5O__alloc(mf, oh, H5O_MSG_NAME, &nm, &idx), 0);
      return idx;
    };

    // Pack all initial nulls in chunk 0 before blocking, then allocate `filler16`
    // (16B) and `post8` (8B) so `filler16` is adjacent to a non-null message.
    for (bool progress = true; progress;) {
      progress = false;
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
            oh->mesg[i].raw_size >= 1) {
          alloc_name(oh->mesg[i].raw_size);
          progress = true;
          break;
        }
      }
    }
    size_t filler_idx = alloc_name(16);
    alloc_name(8);

    // Block chunk 0 so allocating `small_raw_sz` creates chunk 1 via `filler16`.
    ScopedGroup blk(
        H5Gcreate2(mem.get(), "/blk", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(blk.valid());

    size_t target_idx = alloc_name(small_raw_sz);
    ASSERT_GE(oh->nchunks, 2u);
    ASSERT_EQ(oh->mesg[filler_idx].chunkno, 1u);
    ASSERT_EQ(oh->mesg[target_idx].chunkno, 1u);

    // Fill any leftover null in chunk 0 down to exact 0.
    for (bool progress = true; progress;) {
      progress = false;
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
            oh->mesg[i].raw_size >= 1) {
          alloc_name(oh->mesg[i].raw_size);
          progress = true;
          break;
        }
      }
    }

    // Release `filler16` in chunk 1 so chunk 1's only non-null message is target.
    EXPECT_GE(H5O__release_mesg(mf, oh, &oh->mesg[filler_idx], false), 0);
    EXPECT_GE(H5O__condense_header(mf, oh), 0);
    EXPECT_EQ(oh->nchunks, 1u);
  };

  run_move_cont_case(12, "move_cont_12.h5");
  run_move_cont_case(14, "move_cont_14.h5");
  run_move_cont_case(16, "move_cont_16.h5");
}

TEST(H5OallocTest, GapAdjacentNullNoMiddleAndAllocMsgsEdges) {
  // 1) H5O__eliminate_gap with `move_end == move_start && !null_before_gap`
  //    (lines 279, 282-283): place [null_A(12B), null_B(8B)] adjacent and
  //    allocate 10B out of null_A -> 2B gap sits immediately before null_B!
  // 2) Hit oh->nmesgs == oh->alloc_nmesgs inside H5O__alloc_extend_chunk (589)
  //    and H5O__add_gap (165-166) and H5O__move_msgs_forward (1820-1827).
  ScopedFile file = CreateCoreFileWithBounds(
      "gap_adj_edge.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());
  ScopedPlist ocpl = MakeCompactAttrOcpl(128, 64, /*track_times=*/false);
  ScopedGroup g(
      H5Gcreate2(file.get(), "/g", H5P_DEFAULT, ocpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(g.valid());

  H5F_t* f = GetInternalFile(file.get());
  H5O_loc_t* oloc = H5O_get_loc(g.get());
  {
    ScopedApiContext ctx(oloc->addr);
    H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    auto unprotect = absl::MakeCleanup([&]() {
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NAME_ID) {
          EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[i], false), 0);
        }
      }
      EXPECT_GE(H5O__condense_header(f, oh), 0);
      EXPECT_GE(H5O_unprotect(oloc, oh, H5AC__DIRTIED_FLAG), 0);
    });
    auto alloc_name = [&](size_t raw_sz) -> size_t {
      std::string s(raw_sz - 1, 'e');
      H5O_name_t nm = {s.data()};
      size_t idx = 0;
      EXPECT_GE(H5O__alloc(f, oh, H5O_MSG_NAME, &nm, &idx), 0);
      return idx;
    };
    for (bool progress = true; progress;) {
      progress = false;
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
            oh->mesg[i].raw_size >= 1) {
          alloc_name(oh->mesg[i].raw_size);
          progress = true;
          break;
        }
      }
    }

    // Create adjacent nulls: m_a(12B) immediately followed by m_b(8B) + guard.
    size_t m_a = alloc_name(12);
    size_t m_b = alloc_name(8);
    alloc_name(8);
    EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[m_b], false), 0);
    EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[m_a], false), 0);
    // 10B chooses m_a(12B); diff=2 < 4 creates 2B gap right before m_b(null)!
    alloc_name(10);
    EXPECT_EQ(oh->chunk[0].gap, 0u);

    // Next, saturate oh->nmesgs == oh->alloc_nmesgs cleanly to hit line 589
    // inside H5O__alloc_extend_chunk.
    while (oh->nmesgs < oh->alloc_nmesgs) {
      alloc_name(10);
    }
    // Exactly at capacity (`nmesgs == alloc_nmesgs`, 0 nulls) -> extends chunk!
    alloc_name(10);
    EXPECT_GT(oh->alloc_nmesgs, 8u);
  }

  // Exact-match (`24 == 24`, lines 1769-1776) and small-gap split (`26 - 24`,
  // lines 1791-1808) in H5O__move_msgs_forward using committed datatypes with
  // exact 16B CONT slots so chunk 1 has zero stray filler messages.
  {
    ScopedPlist tcpl(H5Pcreate(H5P_DATATYPE_CREATE));
    ASSERT_TRUE(tcpl.valid());
    ASSERT_GE(H5Pset_obj_track_times(tcpl.get(), false), 0);

    // 1) Exact match (24 == 24) with oh->chunk[1].gap == 2 -> lines 1769-1776.
    ScopedType dt_eq(H5Tcopy(H5T_STD_I8LE));
    ASSERT_TRUE(dt_eq.valid());
    ASSERT_GE(
        H5Tcommit2(file.get(), "/dt_eq", dt_eq.get(), H5P_DEFAULT, tcpl.get(),
                   H5P_DEFAULT),
        0);
    H5O_loc_t* loc_eq = H5O_get_loc(dt_eq.get());
    size_t eq_hole = 0;
    {
      ScopedApiContext ctx(loc_eq->addr);
      H5O_t* oh = H5O_protect(loc_eq, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      auto alloc_name = [&](size_t sz) -> size_t {
        std::string s(sz - 1, 'e');
        H5O_name_t nm = {s.data()};
        size_t idx = 0;
        EXPECT_GE(H5O__alloc(f, oh, H5O_MSG_NAME, &nm, &idx), 0);
        return idx;
      };
      for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < oh->nmesgs; ++i) {
          if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
              oh->mesg[i].raw_size >= 1) {
            alloc_name(oh->mesg[i].raw_size);
            progress = true;
            break;
          }
        }
      }
      eq_hole = alloc_name(24);
      size_t eq_cont = alloc_name(16);
      alloc_name(40);
      EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[eq_cont], false), 0);
      EXPECT_GE(H5O_unprotect(loc_eq, oh, H5AC__DIRTIED_FLAG), 0);
    }
    ScopedGroup blk_eq(H5Gcreate2(file.get(), "/blk_eq", H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(blk_eq.valid());
    {
      ScopedApiContext ctx(loc_eq->addr);
      H5O_t* oh = H5O_protect(loc_eq, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      auto alloc_name = [&](size_t sz) -> size_t {
        std::string s(sz - 1, 'q');
        H5O_name_t nm = {s.data()};
        size_t idx = 0;
        EXPECT_GE(H5O__alloc(f, oh, H5O_MSG_NAME, &nm, &idx), 0);
        return idx;
      };
      size_t c1_eq = alloc_name(24);
      size_t c1_gap_src = alloc_name(30);
      alloc_name(40);
      ASSERT_EQ(oh->nchunks, 2u);
      EXPECT_EQ(oh->mesg[c1_eq].chunkno, 1u);
      EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[c1_gap_src], false), 0);
      alloc_name(28);  // Leaves gap == 2 in chunk 1 (`28 > 24`!).
      EXPECT_EQ(oh->chunk[1].gap, 2u);
      EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[eq_hole], false), 0);
      EXPECT_GE(H5O__condense_header(f, oh), 0);
      EXPECT_EQ(oh->chunk[1].gap, 0u);
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NAME_ID) {
          EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[i], false), 0);
        }
      }
      EXPECT_GE(H5O__condense_header(f, oh), 0);
      EXPECT_GE(H5O_unprotect(loc_eq, oh, H5AC__DIRTIED_FLAG), 0);
    }

    // 2) Forward move where 0 < null_msg->raw_size - curr_msg->raw_size < 4
    //    -> adds gap in target chunk (lines 1791-1808).
    ScopedType dt_mgap(H5Tcopy(H5T_STD_I8LE));
    ASSERT_TRUE(dt_mgap.valid());
    ASSERT_GE(
        H5Tcommit2(file.get(), "/dt_mgap", dt_mgap.get(), H5P_DEFAULT,
                   tcpl.get(), H5P_DEFAULT),
        0);
    H5O_loc_t* loc_mg = H5O_get_loc(dt_mgap.get());
    size_t hole26 = 0;
    {
      ScopedApiContext ctx(loc_mg->addr);
      H5O_t* oh = H5O_protect(loc_mg, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      auto alloc_name = [&](size_t sz) -> size_t {
        std::string s(sz - 1, 'a');
        H5O_name_t nm = {s.data()};
        size_t idx = 0;
        EXPECT_GE(H5O__alloc(f, oh, H5O_MSG_NAME, &nm, &idx), 0);
        return idx;
      };
      for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < oh->nmesgs; ++i) {
          if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
              oh->mesg[i].raw_size >= 1) {
            alloc_name(oh->mesg[i].raw_size);
            progress = true;
            break;
          }
        }
      }
      hole26 = alloc_name(26);
      size_t mg_cont = alloc_name(16);
      alloc_name(40);
      EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[mg_cont], false), 0);
      EXPECT_GE(H5O_unprotect(loc_mg, oh, H5AC__DIRTIED_FLAG), 0);
    }
    ScopedGroup blk_mg(H5Gcreate2(file.get(), "/blk_mg", H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(blk_mg.valid());
    {
      ScopedApiContext ctx(loc_mg->addr);
      H5O_t* oh = H5O_protect(loc_mg, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      auto alloc_name = [&](size_t sz) -> size_t {
        std::string s(sz - 1, 'b');
        H5O_name_t nm = {s.data()};
        size_t idx = 0;
        EXPECT_GE(H5O__alloc(f, oh, H5O_MSG_NAME, &nm, &idx), 0);
        return idx;
      };
      alloc_name(24);
      alloc_name(40);
      ASSERT_EQ(oh->nchunks, 2u);
      EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[hole26], false), 0);
      EXPECT_GE(H5O__condense_header(f, oh), 0);
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NAME_ID) {
          EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[i], false), 0);
        }
      }
      EXPECT_GE(H5O__condense_header(f, oh), 0);
      EXPECT_GE(H5O_unprotect(loc_mg, oh, H5AC__DIRTIED_FLAG), 0);
    }
  }

  // Exact swap & split-swap in H5O__move_msgs_forward with gap > 0 in old
  // chunk (lines 1769-1776, 1820-1827, 1854-1861) using isolated H5Tcommit_anon.
  {
    ScopedFile mf = CreateCoreFileWithBounds(
        "eq_gap_anon.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(mf.valid());
    H5F_t* mff = GetInternalFile(mf.get());
    ASSERT_NE(mff, nullptr);
    ScopedPlist tcpl(H5Pcreate(H5P_DATATYPE_CREATE));
    ASSERT_TRUE(tcpl.valid());
    ASSERT_GE(H5Pset_obj_track_times(tcpl.get(), false), 0);
    ASSERT_GE(H5Pset_attr_phase_change(tcpl.get(), 128, 64), 0);
    ScopedType dt_eq_gap(H5Tcopy(H5T_STD_I8LE));
    ASSERT_TRUE(dt_eq_gap.valid());
    ASSERT_GE(
        H5Tcommit_anon(mf.get(), dt_eq_gap.get(), tcpl.get(), H5P_DEFAULT), 0);
    H5O_loc_t* dloc = H5O_get_loc(dt_eq_gap.get());
    size_t c0_cont_src = 0, c0_hole_eq = 0, c0_hole_gt = 0;
    {
      ScopedApiContext ctx(dloc->addr);
      H5O_t* oh = H5O_protect(dloc, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      auto alloc_name = [&](size_t raw_sz) -> size_t {
        std::string s(raw_sz - 1, 'm');
        H5O_name_t nm = {s.data()};
        size_t idx = 0;
        EXPECT_GE(H5O__alloc(mff, oh, H5O_MSG_NAME, &nm, &idx), 0);
        return idx;
      };
      for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < oh->nmesgs; ++i) {
          if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].chunkno == 0 &&
              oh->mesg[i].raw_size >= 1) {
            size_t step = (oh->mesg[i].raw_size > 15) ? 10 : oh->mesg[i].raw_size;
            alloc_name(step);
            progress = true;
            break;
          }
        }
      }
      c0_cont_src = alloc_name(16);
      c0_hole_eq = alloc_name(16);
      c0_hole_gt = alloc_name(32);
      alloc_name(40);  // c0_guard
      ASSERT_EQ(oh->nchunks, 1u);
      EXPECT_GE(H5O_unprotect(dloc, oh, H5AC__DIRTIED_FLAG), 0);
    }
    ScopedType dt_blk(H5Tcopy(H5T_STD_I8LE));
    ASSERT_TRUE(dt_blk.valid());
    ASSERT_GE(
        H5Tcommit_anon(mf.get(), dt_blk.get(), H5P_DEFAULT, H5P_DEFAULT), 0);

    ScopedApiContext ctx(dloc->addr);
    H5O_t* oh = H5O_protect(dloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    auto unprotect = absl::MakeCleanup([&]() {
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NAME_ID) {
          EXPECT_GE(H5O__release_mesg(mff, oh, &oh->mesg[i], false), 0);
        }
      }
      EXPECT_GE(H5O__condense_header(mff, oh), 0);
      EXPECT_GE(H5O_unprotect(dloc, oh, H5AC__DIRTIED_FLAG), 0);
    });
    auto alloc_name = [&](size_t raw_sz) -> size_t {
      std::string s(raw_sz - 1, 'm');
      H5O_name_t nm = {s.data()};
      size_t idx = 0;
      EXPECT_GE(H5O__alloc(mff, oh, H5O_MSG_NAME, &nm, &idx), 0);
      return idx;
    };

    // Create chunk 1: moves c0_cont_src(16) out (leaving 0 nulls in chunk 0!),
    // then add c1_smaller(24), c1_gap_src(42), c1_guard(40).
    alloc_name(40);
    alloc_name(24);
    size_t c1_gap_src = alloc_name(42);
    alloc_name(40);
    ASSERT_EQ(oh->nchunks, 2u);
    EXPECT_EQ(oh->mesg[c0_cont_src].chunkno, 1u);

    // Produce a 2B gap inside chunk 1 (ONLY null is 42B -> 40B leaves 2B gap;
    // size 40 > 32 so it never swaps into c0_hole_eq(16) or c0_hole_gt(32)!).
    EXPECT_GE(H5O__release_mesg(mff, oh, &oh->mesg[c1_gap_src], false), 0);
    alloc_name(40);
    EXPECT_EQ(oh->chunk[1].gap, 2u);

    // Case 1: exact swap (`16 == 16`) with chunk[1].gap == 2 -> lines 1769-1776!
    EXPECT_GE(H5O__release_mesg(mff, oh, &oh->mesg[c0_hole_eq], false), 0);
    EXPECT_GE(H5O__condense_header(mff, oh), 0);
    EXPECT_EQ(oh->chunk[1].gap, 0u);

    // Produce a 2B gap inside chunk 1 again for split-swap (`24 < 32`) +
    // oh->nmesgs == oh->alloc_nmesgs (lines 1820-1827, 1854-1861).
    // Extend chunk 1 to pack alloc_nmesgs BEFORE creating the gap so
    // H5O__alloc_extend_chunk never zeroes chunk[1].gap. Releasing g2_src and
    // allocating 40B in-place keeps oh->nmesgs == oh->alloc_nmesgs.
    ASSERT_EQ(oh->nchunks, 2u);
    size_t g2_src = alloc_name(42);
    alloc_name(40);  // Guard at end of chunk 1.
    while (oh->nmesgs < oh->alloc_nmesgs) {
      alloc_name(40);  // > 32B so only c1_smaller(24) swaps into c0_hole_gt(32).
    }
    EXPECT_EQ(oh->nmesgs, oh->alloc_nmesgs);
    EXPECT_GE(H5O__release_mesg(mff, oh, &oh->mesg[g2_src], false), 0);
    alloc_name(40);  // Reuses g2_src's slot -> 2B gap + keeps nmesgs == alloc_nmesgs!
    EXPECT_EQ(oh->nmesgs, oh->alloc_nmesgs);
    EXPECT_EQ(oh->chunk[1].gap, 2u);
    EXPECT_GE(H5O__release_mesg(mff, oh, &oh->mesg[c0_hole_gt], false), 0);
    EXPECT_GE(H5O__condense_header(mff, oh), 0);
    EXPECT_EQ(oh->chunk[1].gap, 0u);
  }
}

TEST(H5OallocTest, SwmrMoveContinuationUpdatesFlushDependencies) {
  // Exercises H5O__move_msgs_forward lines 1680-1736 where a H5O_CONT message
  // in SWMR write mode moves into a null space in chunk 1 (non-zero, creating
  // fd_parent) and then chunk 0 (destroying fd_parent).
  TempFile tmp("swmr_cont.h5");
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);
  ScopedFile file(
      H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());
  ScopedPlist ocpl = MakeCompactAttrOcpl(128, 64, /*track_times=*/false);
  {
    ScopedGroup g(
        H5Gcreate2(file.get(), "/g", H5P_DEFAULT, ocpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
  }
  // Enable SWMR write BEFORE adding chunks 1..3 so H5O__chunk_add creates
  // flush dependencies (`chk2->fd_parent = chk1`, `chk3->fd_parent = chk2`).
  ASSERT_GE(H5Fstart_swmr_write(file.get()), 0);
  ScopedGroup g(H5Gopen2(file.get(), "/g", H5P_DEFAULT));
  ASSERT_TRUE(g.valid());

  H5F_t* f = GetInternalFile(file.get());
  H5O_loc_t* oloc = H5O_get_loc(g.get());
  ASSERT_NE(f, nullptr);
  ASSERT_NE(oloc, nullptr);

  size_t c0_cont = 0, c0_hole = 0;
  size_t c1_cont = 0, c1_hole = 0;
  size_t c2_cont = 0;
  auto alloc_name = [&](H5O_t* oh, size_t raw_sz) -> size_t {
    std::string s(raw_sz - 1, 'w');
    H5O_name_t nm = {s.data()};
    size_t idx = 0;
    EXPECT_GE(H5O__alloc(f, oh, H5O_MSG_NAME, &nm, &idx), 0);
    return idx;
  };
  auto pack_nulls = [&](H5O_t* oh) {
    for (bool progress = true; progress;) {
      progress = false;
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NULL_ID && oh->mesg[i].raw_size >= 1) {
          alloc_name(oh, oh->mesg[i].raw_size);
          progress = true;
          break;
        }
      }
    }
  };

  {
    ScopedApiContext ctx(oloc->addr);
    H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    pack_nulls(oh);
    c0_hole = alloc_name(oh, 24);
    c0_cont = alloc_name(oh, 16);
    alloc_name(oh, 40);
    EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[c0_cont], false), 0);
    EXPECT_GE(H5O_unprotect(oloc, oh, H5AC__DIRTIED_FLAG), 0);
  }
  ScopedGroup b0(
      H5Gcreate2(file.get(), "/b0", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(b0.valid());

  {
    ScopedApiContext ctx(oloc->addr);
    H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    alloc_name(oh, 80);  // Creates chunk 1, puts CONT->1 into c0_cont!
    c1_hole = alloc_name(oh, 24);
    c1_cont = alloc_name(oh, 16);
    alloc_name(oh, 40);
    EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[c1_cont], false), 0);
    EXPECT_GE(H5O_unprotect(oloc, oh, H5AC__DIRTIED_FLAG), 0);
  }
  ScopedGroup b1(
      H5Gcreate2(file.get(), "/b1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(b1.valid());

  {
    ScopedApiContext ctx(oloc->addr);
    H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    alloc_name(oh, 80);  // Creates chunk 2, puts CONT->2 into c1_cont (`chk2->fd_parent = chk1`)!
    c2_cont = alloc_name(oh, 16);
    alloc_name(oh, 40);
    EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[c2_cont], false), 0);
    EXPECT_GE(H5O_unprotect(oloc, oh, H5AC__DIRTIED_FLAG), 0);
  }
  ScopedGroup b2(
      H5Gcreate2(file.get(), "/b2", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(b2.valid());

  {
    ScopedApiContext ctx(oloc->addr);
    H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(oh, nullptr);
    auto unprotect = absl::MakeCleanup([&]() {
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type->id == H5O_NAME_ID) {
          EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[i], false), 0);
        }
      }
      EXPECT_GE(H5O__condense_header(f, oh), 0);
      EXPECT_GE(H5O_unprotect(oloc, oh, H5AC__DIRTIED_FLAG), 0);
    });
    alloc_name(oh, 80);  // Creates chunk 3, puts CONT->3 into c2_cont (`chk3->fd_parent = chk2`)!
    EXPECT_TRUE(oh->swmr_write);
    ASSERT_EQ(oh->nchunks, 4u);

    // Step 1: Open 24B hole in chunk 1 (`c1_hole`). All non-CONT messages in
    // chunks 2 & 3 are >= 40B, so ONLY CONT->3 (16B) fits -> moves from chunk 2
    // -> chunk 1, destroying `chk2` fd_parent (1708, 1712) & creating `chk1`
    // fd_parent (1724, 1728)!
    EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[c1_hole], false), 0);
    EXPECT_GE(H5O__condense_header(f, oh), 0);

    // Step 2: Open 24B hole in chunk 0 (`c0_hole`). All non-CONT messages in
    // chunks 1..3 are >= 40B, so CONT->2/3 (16B) moves from chunk 1 -> chunk 0
    // (destroying `chk1` fd_parent, skipping create on chunk 0).
    EXPECT_GE(H5O__release_mesg(f, oh, &oh->mesg[c0_hole], false), 0);
    EXPECT_GE(H5O__condense_header(f, oh), 0);
  }
}

TEST(H5OallocTest, Version1ObjectHeaderAllocationAndCondense) {
  // Also cover H5O_VERSION_1 (8-byte aligned, no gaps, no magic/chksum)
  // across multi-chunk allocation and condensation.
  ScopedFile file = CreateCoreFileWithBounds(
      "v1_alloc.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_V18);
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/v1g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  AddBlobAttr(grp.get(), "v1_0", 40);
  ScopedGroup blk(
      H5Gcreate2(file.get(), "/v1blk", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(blk.valid());
  for (int i = 1; i < 6; ++i) {
    AddBlobAttr(grp.get(), ("v1_" + std::to_string(i)).c_str(), 40);
  }
  DeleteAttrs(grp.get(), {"v1_1", "v1_3"});
  AddBlobAttr(grp.get(), "v1_fit", 32);
  DeleteAttrs(grp.get(), {"v1_2", "v1_4", "v1_5", "v1_fit"});

  H5O_info2_t oinfo = {};
  ASSERT_GE(H5Oget_info3(grp.get(), &oinfo, H5O_INFO_NUM_ATTRS), 0);
  EXPECT_EQ(oinfo.num_attrs, 1u);
}

}  // namespace
}  // namespace hdf5
