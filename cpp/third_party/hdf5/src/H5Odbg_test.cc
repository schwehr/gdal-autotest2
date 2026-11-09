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
// Tests object header debugging routines (H5O_debug_id, H5O__debug_real,
// H5O_debug) in third_party/hdf5/src/H5Odbg.c across v1 and v2 object headers,
// status flags, continuation chunks, gaps, and diagnostic error branches.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Odbg.c

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t (H5AC_info_t) and H5O_t (`ndecode_dirtied`). Define NDEBUG
// around hdf5_internal_prelude.h and internal headers so struct layouts match
// `:hdf5`. Note also H5O__assert in H5Odbg.c is compiled out without H5O_DEBUG.
#ifndef NDEBUG
#define H5ODBG_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5ODBG_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ODBG_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

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

template <typename Fn>
std::string CaptureStream(Fn&& fn, herr_t* rc_out) {
  char* buf = nullptr;
  size_t len = 0;
  FILE* stream = open_memstream(&buf, &len);
  if (stream == nullptr) {
    *rc_out = -1;
    return "";
  }
  absl::Cleanup free_buf = [&] { std::free(buf); };
  *rc_out = fn(stream);
  std::fclose(stream);
  return buf != nullptr ? std::string(buf, len) : std::string();
}

TEST(H5OdbgTest, DebugIdFormatsRegisteredMessageTypes) {
  ScopedFile file = CreateInMemoryFile("odbg_id.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  std::time_t now = 1700000000;
  herr_t rc = -1;
  std::string out = CaptureStream(
      [&](FILE* stream) {
        return H5O_debug_id(H5O_MTIME_NEW_ID, f, &now, stream, 2, 20);
      },
      &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("Time:"));

  H5O_refcount_t rc_msg = 3;
  out = CaptureStream(
      [&](FILE* stream) {
        return H5O_debug_id(H5O_REFCOUNT_ID, f, &rc_msg, stream, 0, 18);
      },
      &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("Number of links:"));
}

TEST(H5OdbgTest, DebugV1AndV2RealObjectHeadersWithContinuationsAndFlags) {
  // 1. Version 1 object header (H5F_LIBVER_EARLIEST) + dirty and clean states.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_V18),
              0);
    ScopedFile file(H5Fcreate("odbg_v1.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedGroup grp(H5Gcreate2(file.get(), "g1", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    H5O_loc_t* oloc = H5O_get_loc(grp.get());
    ASSERT_NE(oloc, nullptr);

    {
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      herr_t rc = -1;
      std::string dirty_out = CaptureStream(
          [&](FILE* stream) {
            return H5O_debug(f, oloc->addr, stream, 0, 24);
          },
          &rc);
      EXPECT_GE(rc, 0);
      EXPECT_THAT(dirty_out, HasSubstr("Version:"));
      EXPECT_THAT(dirty_out, HasSubstr("TRUE"));
    }

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    {
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      herr_t rc = -1;
      std::string clean_out = CaptureStream(
          [&](FILE* stream) {
            return H5O_debug(f, oloc->addr, stream, 0, 24);
          },
          &rc);
      EXPECT_GE(rc, 0);
      EXPECT_THAT(clean_out, HasSubstr("FALSE"));
    }
  }

  // 2. Version 2 object headers covering tracked/indexed attr order, custom
  //    phase change, disabled vs enabled timestamps, continuation chunks, NULL
  //    messages, and lazy message decode on RDONLY reopen.
  {
    TempFile path("odbg_v2_reopen.h5");
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);

    haddr_t full_flags_addr = HADDR_UNDEF;
    haddr_t no_times_addr = HADDR_UNDEF;
    {
      ScopedFile file(
          H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(file.valid());

      // Object A: all v2 flags set + multiple attributes -> continuation chunks
      // and deleted null messages.
      ScopedPlist ocpl_full(H5Pcreate(H5P_GROUP_CREATE));
      ASSERT_TRUE(ocpl_full.valid());
      ASSERT_GE(H5Pset_attr_creation_order(
                    ocpl_full.get(),
                    H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
                0);
      ASSERT_GE(H5Pset_attr_phase_change(ocpl_full.get(), 20, 12), 0);
      ASSERT_GE(H5Pset_obj_track_times(ocpl_full.get(), true), 0);

      ScopedGroup grp_full(H5Gcreate2(file.get(), "grp_full", H5P_DEFAULT,
                                      ocpl_full.get(), H5P_DEFAULT));
      ASSERT_TRUE(grp_full.valid());
      H5O_loc_t* oloc_full = H5O_get_loc(grp_full.get());
      ASSERT_NE(oloc_full, nullptr);
      full_flags_addr = oloc_full->addr;

      // Object B: v2 header with timestamps disabled and default phase change.
      // Creating grp_min + a dataset immediately after grp_full occupies the
      // adjacent file space so later attributes on grp_full cannot extend
      // chunk 0 in place and must allocate a continuation chunk (Chunk 1).
      ScopedPlist ocpl_min(H5Pcreate(H5P_GROUP_CREATE));
      ASSERT_TRUE(ocpl_min.valid());
      ASSERT_GE(H5Pset_obj_track_times(ocpl_min.get(), false), 0);
      ScopedGroup grp_min(H5Gcreate2(file.get(), "grp_min", H5P_DEFAULT,
                                     ocpl_min.get(), H5P_DEFAULT));
      ASSERT_TRUE(grp_min.valid());
      H5O_loc_t* oloc_min = H5O_get_loc(grp_min.get());
      ASSERT_NE(oloc_min, nullptr);
      no_times_addr = oloc_min->addr;

      ScopedSpace scalar(H5Screate(H5S_SCALAR));
      ASSERT_TRUE(scalar.valid());
      ScopedDataset blocker(H5Dcreate2(file.get(), "blocker", H5T_NATIVE_INT32,
                                       scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT));
      ASSERT_TRUE(blocker.valid());
      const int val = 7;
      ASSERT_GE(H5Dwrite(blocker.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                         H5P_DEFAULT, &val),
                0);
      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

      for (int i = 0; i < 18; ++i) {
        std::string name = absl::StrCat("attr_", i);
        ScopedAttr a(H5Acreate2(grp_full.get(), name.c_str(), H5T_NATIVE_INT32,
                                scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
        ASSERT_TRUE(a.valid());
        ASSERT_GE(H5Awrite(a.get(), H5T_NATIVE_INT32, &i), 0);
      }
      // Remove several attributes to create NULL messages / chunk gaps.
      EXPECT_GE(H5Adelete(grp_full.get(), "attr_2"), 0);
      EXPECT_GE(H5Adelete(grp_full.get(), "attr_5"), 0);
      EXPECT_GE(H5Adelete(grp_full.get(), "attr_9"), 0);
    }

    // Reopen RDONLY so cache has not decoded native messages yet -> exercises
    // H5O_LOAD_NATIVE inside H5O__debug_real plus continuation chunks.
    ScopedFile reopened(H5Fopen(path.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(reopened.valid());
    H5F_t* f = UnwrapFile(reopened.get());
    ASSERT_NE(f, nullptr);

    {
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      herr_t rc = -1;
      std::string out_full = CaptureStream(
          [&](FILE* stream) {
            return H5O_debug(f, full_flags_addr, stream, 0, 32);
          },
          &rc);
      EXPECT_GE(rc, 0);
      EXPECT_THAT(out_full, HasSubstr("Attribute creation order tracked:"));
      EXPECT_THAT(out_full, HasSubstr("Access Time:"));
      EXPECT_THAT(out_full, HasSubstr("Max. compact attributes:"));
      EXPECT_THAT(out_full, HasSubstr("Min. dense attributes:"));
      EXPECT_THAT(out_full, HasSubstr("Chunk 1..."));
      EXPECT_THAT(out_full, HasSubstr("continuation"));

      std::string out_min = CaptureStream(
          [&](FILE* stream) {
            return H5O_debug(f, no_times_addr, stream, 0, 32);
          },
          &rc);
      EXPECT_GE(rc, 0);
      EXPECT_THAT(out_min, HasSubstr("Disabled"));
      EXPECT_THAT(out_min, HasSubstr("Default"));
    }

    // Invalid address causes H5O_protect to fail inside H5O_debug.
    {
      ScopedErrorSilence silence;
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      herr_t rc = 0;
      std::string out_err = CaptureStream(
          [&](FILE* stream) {
            return H5O_debug(f, full_flags_addr + 0x100000ULL, stream, 0, 24);
          },
          &rc);
      EXPECT_LT(rc, 0);
    }
  }
}

TEST(H5OdbgTest, DebugRealCoversAllMessageFlagsAnomaliesAndDecodeError) {
  ScopedFile file = CreateInMemoryFile("odbg_synthetic.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  // Backing byte array large enough that out-of-range offsets around
  // `chunk[0].image` stay strictly inside the same C++ vector buffer (no UB).
  std::vector<uint8_t> image_backing(256, 0U);
  uint8_t* img0_start = image_backing.data() + 32;
  uint8_t* img1_start = image_backing.data() + 144;

  // Encode a valid MTIME_NEW native message into img0_start for lazy decode.
  std::time_t sample_time = 1700000123;
  ASSERT_GE(
      H5O_msg_encode(f, H5O_MTIME_NEW_ID, false, img0_start, &sample_time), 0);
  const size_t mtime_raw_sz =
      H5O_msg_raw_size(f, H5O_MTIME_NEW_ID, false, &sample_time);

  // Build synthetic chunks[2]; oh.nchunks = 1 intentionally while initializing
  // chunks[1] so chunkno == 1 exercises the "*** BAD CHUNK NUMBER" diagnostic
  // safely without out-of-bounds memory access.
  std::vector<H5O_chunk_t> chunks(2);
  chunks[0].addr = 0x2000;
  chunks[0].size = 96;
  chunks[0].gap = 4;
  chunks[0].image = img0_start;
  chunks[1].addr = 0x3000;
  chunks[1].size = 48;
  chunks[1].gap = 0;
  chunks[1].image = img1_start;

  H5O_msg_class_t bad_msg_type = {};
  bad_msg_type.id = H5O_MSG_TYPES;
  bad_msg_type.name = "bogus_id";

  const uint8_t single_flags[] = {
      0,
      H5O_MSG_FLAG_CONSTANT,
      H5O_MSG_FLAG_SHARED,
      H5O_MSG_FLAG_DONTSHARE,
      H5O_MSG_FLAG_FAIL_IF_UNKNOWN_AND_OPEN_FOR_WRITE,
      H5O_MSG_FLAG_MARK_IF_UNKNOWN,
      H5O_MSG_FLAG_SHAREABLE,
      H5O_MSG_FLAG_FAIL_IF_UNKNOWN_ALWAYS,
      H5O_MSG_FLAG_BITS,
  };

  std::vector<H5O_mesg_t> mesgs;
  // 0. Lazy-decoded MTIME_NEW message (dirty = true, flags = CONSTANT).
  {
    H5O_mesg_t m = {};
    m.type = H5O_MSG_MTIME_NEW;
    m.dirty = true;
    m.flags = H5O_MSG_FLAG_CONSTANT;
    m.chunkno = 0;
    m.native = nullptr;
    m.raw = img0_start;
    m.raw_size = mtime_raw_sz;
    mesgs.push_back(m);
  }
  // 1..9. One H5O_MSG_NULL message per flag configuration.
  for (uint8_t fl : single_flags) {
    H5O_mesg_t m = {};
    m.type = H5O_MSG_NULL;
    m.dirty = false;
    m.flags = fl;
    m.chunkno = 0;
    m.native = nullptr;
    m.raw = img0_start + 8;
    m.raw_size = 4;
    mesgs.push_back(m);
  }
  // 10. Bad message ID (>= NELMTS(H5O_msg_class_g)).
  {
    H5O_mesg_t m = {};
    m.type = &bad_msg_type;
    m.chunkno = 0;
    m.raw = img0_start + 16;
    m.raw_size = 4;
    mesgs.push_back(m);
  }
  // 11. Bad chunk number (chunkno == 1 while nchunks == 1).
  {
    H5O_mesg_t m = {};
    m.type = H5O_MSG_NULL;
    m.chunkno = 1;
    m.raw = img1_start;
    m.raw_size = 4;
    mesgs.push_back(m);
  }
  // 12. Raw address < chunk[0].image (stays within image_backing).
  {
    H5O_mesg_t m = {};
    m.type = H5O_MSG_NULL;
    m.chunkno = 0;
    m.raw = image_backing.data() + 16;
    m.raw_size = 4;
    mesgs.push_back(m);
  }
  // 13. Raw end > chunk[0].image + chunk[0].size (stays within image_backing).
  {
    H5O_mesg_t m = {};
    m.type = H5O_MSG_NULL;
    m.chunkno = 0;
    m.raw = img0_start + chunks[0].size;
    m.raw_size = 8;
    mesgs.push_back(m);
  }

  H5O_t oh = {};
  oh.cache_info.is_dirty = true;
  oh.sizeof_size = 8;
  oh.sizeof_addr = 8;
  oh.nlink = 2;
  oh.version = H5O_VERSION_2;
  // Set unknown v2 object header status flag bit (0x40) + TIMES + PHASE_CHANGE.
  oh.flags = static_cast<uint8_t>(H5O_HDR_CHUNK0_1 | H5O_HDR_STORE_TIMES |
                                  H5O_HDR_ATTR_STORE_PHASE_CHANGE | 0x40U);
  oh.atime = 1700000001;
  oh.mtime = 1700000002;
  oh.ctime = 1700000003;
  oh.btime = 1700000004;
  oh.max_compact = 10;
  oh.min_dense = 5;
  oh.nmesgs = mesgs.size();
  oh.alloc_nmesgs = mesgs.size();
  oh.mesg = mesgs.data();
  oh.nchunks = 1;
  oh.alloc_nchunks = chunks.size();
  oh.chunk = chunks.data();

  absl::Cleanup free_decoded_natives = [&] {
    for (auto& m : mesgs) {
      if (m.native != nullptr) {
        H5O_msg_free(m.type->id, m.native);
        m.native = nullptr;
      }
    }
  };

  herr_t rc = -1;
  // Intentionally pass addr != chunks[0].addr to trigger "*** WRONG ADDRESS".
  std::string out = CaptureStream(
      [&](FILE* stream) {
        return H5O__debug_real(f, &oh, chunks[0].addr + 8, stream, 0, 28);
      },
      &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("*** UNKNOWN OBJECT HEADER STATUS FLAG"));
  EXPECT_THAT(out, HasSubstr("*** WRONG ADDRESS FOR CHUNK #0!"));
  EXPECT_THAT(out, HasSubstr("*** BAD MESSAGE ID"));
  EXPECT_THAT(out, HasSubstr("<none>"));
  EXPECT_THAT(out, HasSubstr("<C, S, DS, FIUW, MIU, WU, SA, FIUA>"));
  EXPECT_THAT(out, HasSubstr("*** BAD CHUNK NUMBER"));
  EXPECT_THAT(out, HasSubstr("*** BAD MESSAGE RAW ADDRESS"));
  EXPECT_THAT(out, HasSubstr("<No info for this message>"));
  EXPECT_THAT(out, HasSubstr("*** TOTAL SIZE DOES NOT MATCH ALLOCATED SIZE!"));

  // Also cover H5O_LOAD_NATIVE decode failure returning FAIL from
  // H5O__debug_real.
  {
    ScopedErrorSilence silence;
    std::vector<uint8_t> corrupt_raw = {0x7FU, 0x00U, 0x00U, 0x00U};
    H5O_chunk_t corrupt_chunk = {};
    corrupt_chunk.addr = 0x4000;
    corrupt_chunk.size = 64;
    corrupt_chunk.image = corrupt_raw.data();

    H5O_mesg_t corrupt_msg = {};
    corrupt_msg.type = H5O_MSG_DRVINFO;
    corrupt_msg.chunkno = 0;
    corrupt_msg.native = nullptr;
    corrupt_msg.raw = corrupt_raw.data();
    corrupt_msg.raw_size = corrupt_raw.size();

    H5O_t bad_oh = {};
    bad_oh.sizeof_size = 8;
    bad_oh.sizeof_addr = 8;
    bad_oh.version = H5O_VERSION_1;
    bad_oh.nmesgs = 1;
    bad_oh.alloc_nmesgs = 1;
    bad_oh.mesg = &corrupt_msg;
    bad_oh.nchunks = 1;
    bad_oh.alloc_nchunks = 1;
    bad_oh.chunk = &corrupt_chunk;

    herr_t bad_rc = 0;
    std::string unused = CaptureStream(
        [&](FILE* stream) {
          return H5O__debug_real(f, &bad_oh, corrupt_chunk.addr, stream, 0, 20);
        },
        &bad_rc);
    EXPECT_LT(bad_rc, 0);
  }
}

}  // namespace
}  // namespace hdf5
