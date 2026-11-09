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
// Tests fractal heap huge object management across direct/indirect ID formats,
// filtered and unfiltered pipelines, persisted v2 B-tree reopen paths, ID
// wraparound, removal, termination, and heap deletion
// (third_party/hdf5/src/H5HFhuge.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HFhuge.c

#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t (H5AC_info_t) inside internal fractal heap structs.
#ifndef NDEBUG
#define H5HFHUGE_TEST_RESTORE_UNDEF_NDEBUG 1
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
#define H5HF_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5HFpkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#include "third_party/hdf5/src/H5Zprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5HFHUGE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HFHUGE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;

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

H5HF_create_t MakeHeapParams(uint16_t id_len, uint32_t max_man_size = 64,
                             unsigned width = 4, size_t start_block_size = 64,
                             size_t max_direct_size = 256,
                             unsigned max_index = 12) {
  H5HF_create_t cparam{};
  cparam.managed.width = width;
  cparam.managed.start_block_size = start_block_size;
  cparam.managed.max_direct_size = max_direct_size;
  cparam.managed.max_index = max_index;
  cparam.managed.start_root_rows = 1;
  cparam.checksum_dblocks = true;
  cparam.max_man_size = max_man_size;
  cparam.id_len = id_len;
  return cparam;
}

std::vector<uint8_t> MakePayload(size_t len, uint8_t seed) {
  std::vector<uint8_t> buf(len);
  for (size_t i = 0; i < len; ++i) {
    buf[i] = static_cast<uint8_t>((seed + (i / 4) * 13) & 0xffU);
  }
  return buf;
}

// Custom toggleable filter used to trigger reverse-pipeline decode failure cleanly.
constexpr H5Z_filter_t kReverseFailFilterId = H5Z_FILTER_RESERVED + 31;
bool g_fail_decode = false;

size_t ReverseFailFilter(unsigned int flags, size_t /*cd_nelmts*/,
                         const unsigned int /*cd_values*/[], size_t nbytes,
                         size_t* /*buf_size*/, void** /*buf*/) {
  if ((flags & H5Z_FLAG_REVERSE) && g_fail_decode) {
    return 0;
  }
  return nbytes;
}

struct ModeConfig {
  uint16_t id_len;
  bool filtered;
  const char* label;
};

class H5HFhugeModesTest : public ::testing::TestWithParam<ModeConfig> {};

TEST_P(H5HFhugeModesTest, CreateInsertQueryModifyReopenRemoveAndDelete) {
  const ModeConfig cfg = GetParam();
  std::vector<uint8_t> image;
  haddr_t persist_heap_addr = HADDR_UNDEF;
  std::vector<uint8_t> persist_id_keep;
  std::vector<uint8_t> persist_id_remove;
  const std::vector<uint8_t> obj1 = MakePayload(256, 0x11U);
  const std::vector<uint8_t> obj2 = MakePayload(320, 0x44U);
  const std::vector<uint8_t> obj3 = MakePayload(192, 0x77U);

  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_create_t cparam = MakeHeapParams(cfg.id_len);
    absl::Cleanup reset_pline = [&] {
      if (cparam.pline.nused > 0) {
        H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
      }
    };
    if (cfg.filtered) {
      unsigned int cd_vals[1] = {6};
      ASSERT_GE(H5Z_append(&cparam.pline, H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL,
                           1, cd_vals),
                0);
    }

    // 1. Heap A: test create/insert/query/read/op/write/remove/term(nobjs==0)
    //    followed by re-inserting and deleting the non-empty heap.
    {
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      H5HF_hdr_t* hdr = fh->hdr;
      haddr_t heap_addr = hdr->heap_addr;
      bool closed = false;
      absl::Cleanup cleanup_fh = [&] {
        if (!closed && fh != nullptr) {
          H5HF_close(fh);
        }
      };

      const bool expect_direct = (cfg.id_len == 1 || cfg.id_len >= 64);
      EXPECT_EQ(hdr->huge_ids_direct, expect_direct);

      std::vector<uint8_t> id1(hdr->id_len, 0);
      std::vector<uint8_t> id2(hdr->id_len, 0);
      ASSERT_GE(H5HF_insert(fh, obj1.size(), obj1.data(), id1.data()), 0);
      ASSERT_GE(H5HF_insert(fh, obj2.size(), obj2.data(), id2.data()), 0);
      EXPECT_EQ(hdr->huge_nobjs, 2U);

      size_t got_len = 0;
      EXPECT_GE(H5HF_get_obj_len(fh, id1.data(), &got_len), 0);
      EXPECT_EQ(got_len, obj1.size());

      hsize_t got_off = 0;
      EXPECT_GE(H5HF_get_obj_off(fh, id1.data(), &got_off), 0);
      EXPECT_NE(got_off, HADDR_UNDEF);

      std::vector<uint8_t> read_buf(obj1.size(), 0);
      EXPECT_GE(H5HF_read(fh, id1.data(), read_buf.data()), 0);
      EXPECT_THAT(read_buf, ElementsAreArray(obj1));

      std::vector<uint8_t> op_buf;
      auto copy_op = [](const void* obj, size_t len, void* op_data) -> herr_t {
        const auto* p = static_cast<const uint8_t*>(obj);
        static_cast<std::vector<uint8_t>*>(op_data)->assign(p, p + len);
        return 0;
      };
      EXPECT_GE(H5HF_op(fh, id2.data(), copy_op, &op_buf), 0);
      EXPECT_THAT(op_buf, ElementsAreArray(obj2));

      // Write modifies unfiltered huge objects and fails cleanly on filtered.
      const std::vector<uint8_t> updated1 = MakePayload(obj1.size(), 0x99U);
      bool id_changed = false;
      if (!cfg.filtered) {
        EXPECT_GE(H5HF_write(fh, id1.data(), &id_changed, updated1.data()), 0);
        std::fill(read_buf.begin(), read_buf.end(), 0);
        EXPECT_GE(H5HF_read(fh, id1.data(), read_buf.data()), 0);
        EXPECT_THAT(read_buf, ElementsAreArray(updated1));
      } else {
        ScopedErrorSilence silence;
        EXPECT_LT(H5HF_write(fh, id1.data(), &id_changed, updated1.data()), 0);
      }

      // Remove id1 and verify NOTFOUND across huge accessors (and remove).
      EXPECT_GE(H5HF_remove(fh, id1.data()), 0);
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5HF_remove(fh, id1.data()), 0);
        if (!expect_direct) {
          size_t miss_len = 0;
          hsize_t miss_off = 0;
          EXPECT_LT(H5HF_get_obj_len(fh, id1.data(), &miss_len), 0);
          EXPECT_LT(H5HF_get_obj_off(fh, id1.data(), &miss_off), 0);
          EXPECT_LT(H5HF_read(fh, id1.data(), read_buf.data()), 0);
          EXPECT_LT(H5HF_op(fh, id1.data(), copy_op, &op_buf), 0);
          if (!cfg.filtered) {
            EXPECT_LT(H5HF_write(fh, id1.data(), &id_changed, updated1.data()),
                      0);
          }
        }
      }

      // Remove remaining object so huge_nobjs reaches 0, then close so
      // H5HF__huge_term deletes the v2 B-tree and resets huge_bt2_addr.
      EXPECT_GE(H5HF_remove(fh, id2.data()), 0);
      EXPECT_EQ(hdr->huge_nobjs, 0U);
      EXPECT_GE(H5HF_close(fh), 0);
      closed = true;

      // Reopen heap A, verify huge_bt2_addr was cleared, insert obj1, and delete
      // the heap immediately via H5HF_delete -> H5HF__huge_delete.
      H5HF_t* fh_again = H5HF_open(f, heap_addr);
      ASSERT_NE(fh_again, nullptr);
      EXPECT_EQ(fh_again->hdr->huge_bt2_addr, HADDR_UNDEF);
      std::vector<uint8_t> id_re(fh_again->hdr->id_len, 0);
      ASSERT_GE(H5HF_insert(fh_again, obj1.size(), obj1.data(), id_re.data()),
                0);
      EXPECT_GE(H5HF_close(fh_again), 0);
      EXPECT_GE(H5HF_delete(f, heap_addr), 0);
    }

    // 2. Heap B: populate 2 huge objects, close, and flush image so on reopen
    //    hdr->huge_bt2 starts as NULL in every H5HF__huge_* entry point.
    {
      H5HF_t* fh_p = H5HF_create(f, &cparam);
      ASSERT_NE(fh_p, nullptr);
      persist_heap_addr = fh_p->hdr->heap_addr;
      persist_id_keep.resize(fh_p->hdr->id_len);
      persist_id_remove.resize(fh_p->hdr->id_len);
      ASSERT_GE(H5HF_insert(fh_p, obj1.size(), obj1.data(),
                            persist_id_keep.data()),
                0);
      ASSERT_GE(H5HF_insert(fh_p, obj2.size(), obj2.data(),
                            persist_id_remove.data()),
                0);
      EXPECT_GE(H5HF_close(fh_p), 0);
    }

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  // Reopen separate copies of the flushed image to hit hdr->huge_bt2 == NULL
  // in get_obj_len, get_obj_off, op_real(read), write, remove, and insert!
  auto open_image = [&](ScopedFile& out_file, ScopedPlist& out_fapl) {
    out_fapl = MakeCoreFapl();
    ASSERT_TRUE(out_fapl.valid());
    ASSERT_GE(
        H5Pset_file_image(out_fapl.get(), image.data(), image.size()), 0);
    out_file.reset(H5Fopen("huge_reopen.h5", H5F_ACC_RDWR, out_fapl.get()));
    ASSERT_TRUE(out_file.valid());
  };

  // Pass 1: H5HF__huge_get_obj_len opens huge_bt2 on demand.
  {
    ScopedPlist fapl;
    ScopedFile f2_sc;
    open_image(f2_sc, fapl);
    ScopedApiContext ctx2;
    H5HF_t* fh = H5HF_open(GetFilePtr(f2_sc.get()), persist_heap_addr);
    ASSERT_NE(fh, nullptr);
    EXPECT_EQ(fh->hdr->huge_bt2, nullptr);
    size_t len = 0;
    EXPECT_GE(H5HF_get_obj_len(fh, persist_id_keep.data(), &len), 0);
    EXPECT_EQ(len, obj1.size());
    EXPECT_GE(H5HF_close(fh), 0);
  }

  // Pass 2: H5HF__huge_get_obj_off opens huge_bt2 on demand.
  {
    ScopedPlist fapl;
    ScopedFile f2_sc;
    open_image(f2_sc, fapl);
    ScopedApiContext ctx2;
    H5HF_t* fh = H5HF_open(GetFilePtr(f2_sc.get()), persist_heap_addr);
    ASSERT_NE(fh, nullptr);
    hsize_t off = 0;
    EXPECT_GE(H5HF_get_obj_off(fh, persist_id_keep.data(), &off), 0);
    EXPECT_NE(off, HADDR_UNDEF);
    EXPECT_GE(H5HF_close(fh), 0);
  }

  // Pass 3: H5HF__huge_op_real opens huge_bt2 on demand.
  {
    ScopedPlist fapl;
    ScopedFile f2_sc;
    open_image(f2_sc, fapl);
    ScopedApiContext ctx2;
    H5HF_t* fh = H5HF_open(GetFilePtr(f2_sc.get()), persist_heap_addr);
    ASSERT_NE(fh, nullptr);
    std::vector<uint8_t> got(obj1.size(), 0);
    EXPECT_GE(H5HF_read(fh, persist_id_keep.data(), got.data()), 0);
    EXPECT_THAT(got, ElementsAreArray(obj1));
    EXPECT_GE(H5HF_close(fh), 0);
  }

  // Pass 4: H5HF__huge_write opens huge_bt2 on demand (for unfiltered).
  if (!cfg.filtered) {
    ScopedPlist fapl;
    ScopedFile f2_sc;
    open_image(f2_sc, fapl);
    ScopedApiContext ctx2;
    H5HF_t* fh = H5HF_open(GetFilePtr(f2_sc.get()), persist_heap_addr);
    ASSERT_NE(fh, nullptr);
    const std::vector<uint8_t> mod = MakePayload(obj1.size(), 0x55U);
    bool id_changed = false;
    EXPECT_GE(H5HF_write(fh, persist_id_keep.data(), &id_changed, mod.data()),
              0);
    EXPECT_GE(H5HF_close(fh), 0);
  }

  // Pass 5: H5HF__huge_remove opens huge_bt2 on demand.
  {
    ScopedPlist fapl;
    ScopedFile f2_sc;
    open_image(f2_sc, fapl);
    ScopedApiContext ctx2;
    H5HF_t* fh = H5HF_open(GetFilePtr(f2_sc.get()), persist_heap_addr);
    ASSERT_NE(fh, nullptr);
    EXPECT_GE(H5HF_remove(fh, persist_id_remove.data()), 0);
    EXPECT_GE(H5HF_close(fh), 0);
  }

  // Pass 6: H5HF__huge_insert opens existing huge_bt2 when already defined.
  {
    ScopedPlist fapl;
    ScopedFile f2_sc;
    open_image(f2_sc, fapl);
    ScopedApiContext ctx2;
    H5HF_t* fh = H5HF_open(GetFilePtr(f2_sc.get()), persist_heap_addr);
    ASSERT_NE(fh, nullptr);
    std::vector<uint8_t> id3(fh->hdr->id_len, 0);
    EXPECT_GE(H5HF_insert(fh, obj3.size(), obj3.data(), id3.data()), 0);
    EXPECT_GE(H5HF_close(fh), 0);
  }
}

INSTANTIATE_TEST_SUITE_P(
    AllHugeModes, H5HFhugeModesTest,
    ::testing::Values(
        ModeConfig{0, false, "IndirectUnfiltered"},
        ModeConfig{1, false, "DirectUnfiltered"},
        ModeConfig{0, true, "IndirectFiltered"},
        ModeConfig{1, true, "DirectFiltered"}),
    [](const ::testing::TestParamInfo<ModeConfig>& info) {
      return info.param.label;
    });

TEST(H5HFhugeTest, IndirectIdMaxWidthBranchAndIdWraparoundAndFilterReadFail) {
  const H5Z_class2_t rev_filter = {
      .version = H5Z_CLASS_T_VERS,
      .id = kReverseFailFilterId,
      .encoder_present = 1,
      .decoder_present = 1,
      .name = "test_reverse_fail_filter",
      .can_apply = nullptr,
      .set_local = nullptr,
      .filter = ReverseFailFilter,
  };
  g_fail_decode = false;
  ASSERT_GE(H5Zregister(&rev_filter), 0);
  absl::Cleanup unreg = [&] {
    g_fail_decode = false;
    H5Zunregister(kReverseFailFilterId);
  };

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  // 1. Indirect unfiltered heap with (id_len - 1) >= sizeof(hsize_t) so
  //    huge_max_id == HSIZET_MAX branch in H5HF__huge_init runs cleanly, and test
  //    reaching huge_max_id -> huge_ids_wrapped = true -> failure on next insert.
  {
    H5HF_create_t cparam = MakeHeapParams(/*id_len=*/12, /*max_man_size=*/64);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    absl::Cleanup close_fh = [&] { H5HF_close(fh); };

    EXPECT_FALSE(fh->hdr->huge_ids_direct);
    EXPECT_EQ(fh->hdr->huge_id_size, sizeof(hsize_t));
    EXPECT_EQ(fh->hdr->huge_max_id, HSIZET_MAX);

    fh->hdr->huge_next_id = fh->hdr->huge_max_id - 1;
    std::vector<uint8_t> id(fh->hdr->id_len, 0);
    std::vector<uint8_t> data = MakePayload(128, 0x22U);
    ASSERT_GE(H5HF_insert(fh, data.size(), data.data(), id.data()), 0);
    EXPECT_TRUE(fh->hdr->huge_ids_wrapped);

    ScopedErrorSilence silence;
    EXPECT_LT(H5HF_insert(fh, data.size(), data.data(), id.data()), 0);
  }

  // 2. Reverse filter failure during H5HF_read / H5HF_op frees read_buf in
  //    H5HF__huge_op_real cleanly, and callback failure inside H5HF__huge_op_real
  //    frees read_buf without leaks.
  {
    H5HF_create_t cparam = MakeHeapParams(/*id_len=*/0, /*max_man_size=*/64);
    absl::Cleanup reset_pline = [&] {
      if (cparam.pline.nused > 0) {
        H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
      }
    };
    ASSERT_GE(H5Z_append(&cparam.pline, kReverseFailFilterId,
                         H5Z_FLAG_MANDATORY, 0, nullptr),
              0);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    absl::Cleanup close_fh = [&] { H5HF_close(fh); };

    std::vector<uint8_t> id(fh->hdr->id_len, 0);
    std::vector<uint8_t> data = MakePayload(160, 0x33U);
    ASSERT_GE(H5HF_insert(fh, data.size(), data.data(), id.data()), 0);

    std::vector<uint8_t> out(data.size(), 0);
    {
      ScopedErrorSilence silence;
      g_fail_decode = true;
      EXPECT_LT(H5HF_read(fh, id.data(), out.data()), 0);
      g_fail_decode = false;

      auto fail_op = [](const void*, size_t, void*) -> herr_t { return -1; };
      EXPECT_LT(H5HF_op(fh, id.data(), fail_op, nullptr), 0);
    }

    EXPECT_GE(H5HF_read(fh, id.data(), out.data()), 0);
    EXPECT_THAT(out, ElementsAreArray(data));
  }
}

}  // namespace
}  // namespace hdf5
