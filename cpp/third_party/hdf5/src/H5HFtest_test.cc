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
// Tests internal fractal heap parameter comparison/retrieval, doubling-table
// inspection, and ID/tiny/huge testing helpers in
// third_party/hdf5/src/H5HFtest.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HFtest.c

#include <array>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
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
#define H5HF_FRIEND
#define H5HF_TESTING
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5HFpkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#include "third_party/hdf5/src/H5Zprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5HF_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HF_TEST_RESTORE_UNDEF_NDEBUG
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

H5F_t* GetFilePtr(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5HF_create_t MakeHeapCreateParams(
    uint16_t id_len = 0, uint32_t max_man_size = 512, unsigned width = 4,
    size_t start_block_size = 512, size_t max_direct_size = 4096,
    unsigned max_index = 20, unsigned start_root_rows = 1) {
  H5HF_create_t cparam{};
  cparam.managed.width = width;
  cparam.managed.start_block_size = start_block_size;
  cparam.managed.max_direct_size = max_direct_size;
  cparam.managed.max_index = max_index;
  cparam.managed.start_root_rows = start_root_rows;
  cparam.checksum_dblocks = true;
  cparam.max_man_size = max_man_size;
  cparam.id_len = id_len;
  return cparam;
}

TEST(H5HFtestTest, GetCparamTestRecoversAutoDirectExplicitAndFiltered) {
  haddr_t filtered_heap_addr = HADDR_UNDEF;
  std::vector<uint8_t> image;

  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    // 1. id_len=0 (auto managed ID) round-trips as id_len=0.
    //    id_len=1 (direct huge ID) round-trips as id_len=1.
    //    id_len=32 (explicit custom ID) round-trips as id_len=32.
    for (uint16_t req_id_len : {uint16_t{0}, uint16_t{1}, uint16_t{32}}) {
      H5HF_create_t cparam = MakeHeapCreateParams(req_id_len);
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

      H5HF_create_t got{};
      auto reset_got_pline = absl::MakeCleanup([&]() {
        if (got.pline.nused > 0) {
          H5O_msg_reset(H5O_PLINE_ID, &got.pline);
        }
      });
      EXPECT_GE(H5HF_get_cparam_test(fh, &got), 0);
      EXPECT_EQ(got.id_len, req_id_len);
      EXPECT_EQ(H5HF_cmp_cparam_test(&cparam, &got), 0);
    }

    // 2. Filtered heap with multi-filter I/O pipeline round-trips identically.
    H5HF_create_t filt_cparam = MakeHeapCreateParams(/*id_len=*/0);
    auto reset_filt_pline = absl::MakeCleanup([&]() {
      if (filt_cparam.pline.nused > 0) {
        H5O_msg_reset(H5O_PLINE_ID, &filt_cparam.pline);
      }
    });
    unsigned deflate_level = 6;
    ASSERT_GE(H5Z_append(&filt_cparam.pline, H5Z_FILTER_DEFLATE,
                         H5Z_FLAG_OPTIONAL, 1, &deflate_level),
              0);
    ASSERT_GE(H5Z_append(&filt_cparam.pline, H5Z_FILTER_FLETCHER32,
                         H5Z_FLAG_MANDATORY, 0, nullptr),
              0);

    H5HF_t* filt_fh = H5HF_create(f, &filt_cparam);
    ASSERT_NE(filt_fh, nullptr);
    auto close_filt = absl::MakeCleanup([&]() {
      if (filt_fh != nullptr) {
        H5HF_close(filt_fh);
      }
    });
    ASSERT_GE(H5HF_get_heap_addr(filt_fh, &filtered_heap_addr), 0);

    H5HF_create_t out{};
    auto reset_out_pline = absl::MakeCleanup([&]() {
      if (out.pline.nused > 0) {
        H5O_msg_reset(H5O_PLINE_ID, &out.pline);
      }
    });
    EXPECT_GE(H5HF_get_cparam_test(filt_fh, &out), 0);
    EXPECT_EQ(H5HF_cmp_cparam_test(&filt_cparam, &out), 0);

    EXPECT_GE(H5HF_close(filt_fh), 0);
    filt_fh = nullptr;

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  // 3. Reopen from file image and confirm decoded cparam & filters match.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("reopen_test.h5", H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedApiContext ctx2;
  ASSERT_TRUE(ctx2.valid());
  H5F_t* f2 = GetFilePtr(reopened.get());
  ASSERT_NE(f2, nullptr);

  H5HF_t* reopened_fh = H5HF_open(f2, filtered_heap_addr);
  ASSERT_NE(reopened_fh, nullptr);
  auto close_reopened = absl::MakeCleanup([&]() { H5HF_close(reopened_fh); });

  H5HF_create_t expected = MakeHeapCreateParams(/*id_len=*/0);
  auto reset_expected_pline = absl::MakeCleanup([&]() {
    if (expected.pline.nused > 0) {
      H5O_msg_reset(H5O_PLINE_ID, &expected.pline);
    }
  });
  unsigned deflate_level = 6;
  ASSERT_GE(H5Z_append(&expected.pline, H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL,
                       1, &deflate_level),
            0);
  ASSERT_GE(H5Z_append(&expected.pline, H5Z_FILTER_FLETCHER32,
                       H5Z_FLAG_MANDATORY, 0, nullptr),
            0);

  H5HF_create_t actual{};
  auto reset_actual_pline = absl::MakeCleanup([&]() {
    if (actual.pline.nused > 0) {
      H5O_msg_reset(H5O_PLINE_ID, &actual.pline);
    }
  });
  EXPECT_GE(H5HF_get_cparam_test(reopened_fh, &actual), 0);
  EXPECT_EQ(H5HF_cmp_cparam_test(&expected, &actual), 0);
}

TEST(H5HFtestTest, CmpCparamTestCoversAllFieldOrderingBranches) {
  const H5HF_create_t base = MakeHeapCreateParams(/*id_len=*/16);
  EXPECT_EQ(H5HF_cmp_cparam_test(&base, &base), 0);

  // Verify symmetric -1 / +1 ordering on each non-pipeline creation field.
  auto check_pair = [&](const H5HF_create_t& smaller,
                        const H5HF_create_t& larger) {
    EXPECT_EQ(H5HF_cmp_cparam_test(&smaller, &larger), -1);
    EXPECT_EQ(H5HF_cmp_cparam_test(&larger, &smaller), 1);
  };

  {
    H5HF_create_t mod = base;
    mod.managed.width = base.managed.width * 2;
    check_pair(base, mod);
  }
  {
    H5HF_create_t mod = base;
    mod.managed.start_block_size = base.managed.start_block_size * 2;
    check_pair(base, mod);
  }
  {
    H5HF_create_t mod = base;
    mod.managed.max_direct_size = base.managed.max_direct_size * 2;
    check_pair(base, mod);
  }
  {
    H5HF_create_t mod = base;
    mod.managed.max_index = base.managed.max_index + 2;
    check_pair(base, mod);
  }
  {
    H5HF_create_t mod = base;
    mod.managed.start_root_rows = base.managed.start_root_rows + 1;
    check_pair(base, mod);
  }
  {
    H5HF_create_t mod = base;
    mod.max_man_size = base.max_man_size + 64;
    check_pair(base, mod);
  }
  {
    H5HF_create_t mod = base;
    mod.id_len = base.id_len + 4;
    check_pair(base, mod);
  }

  // Pipeline count comparison (nused).
  H5HF_create_t with_deflate_6 = base;
  auto reset_deflate_6 = absl::MakeCleanup([&]() {
    if (with_deflate_6.pline.nused > 0) {
      H5O_msg_reset(H5O_PLINE_ID, &with_deflate_6.pline);
    }
  });
  unsigned level6 = 6;
  ASSERT_GE(H5Z_append(&with_deflate_6.pline, H5Z_FILTER_DEFLATE,
                       H5Z_FLAG_OPTIONAL, 1, &level6),
            0);
  check_pair(base, with_deflate_6);

  // Filter ID comparison (DEFLATE < FLETCHER32).
  H5HF_create_t with_fletcher = base;
  auto reset_fletcher = absl::MakeCleanup([&]() {
    if (with_fletcher.pline.nused > 0) {
      H5O_msg_reset(H5O_PLINE_ID, &with_fletcher.pline);
    }
  });
  ASSERT_GE(H5Z_append(&with_fletcher.pline, H5Z_FILTER_FLETCHER32,
                       H5Z_FLAG_OPTIONAL, 0, nullptr),
            0);
  check_pair(with_deflate_6, with_fletcher);

  // Filter flags comparison (mandatory=0 < optional=1).
  H5HF_create_t with_deflate_mand = base;
  auto reset_deflate_mand = absl::MakeCleanup([&]() {
    if (with_deflate_mand.pline.nused > 0) {
      H5O_msg_reset(H5O_PLINE_ID, &with_deflate_mand.pline);
    }
  });
  ASSERT_GE(H5Z_append(&with_deflate_mand.pline, H5Z_FILTER_DEFLATE,
                       H5Z_FLAG_MANDATORY, 1, &level6),
            0);
  check_pair(with_deflate_mand, with_deflate_6);

  // Filter cd_nelmts comparison (1 < 2).
  H5HF_create_t with_deflate_2cd = base;
  auto reset_deflate_2cd = absl::MakeCleanup([&]() {
    if (with_deflate_2cd.pline.nused > 0) {
      H5O_msg_reset(H5O_PLINE_ID, &with_deflate_2cd.pline);
    }
  });
  const std::array<unsigned, 2> two_vals = {6, 1};
  ASSERT_GE(H5Z_append(&with_deflate_2cd.pline, H5Z_FILTER_DEFLATE,
                       H5Z_FLAG_OPTIONAL, two_vals.size(), two_vals.data()),
            0);
  check_pair(with_deflate_6, with_deflate_2cd);

  // Filter cd_values[v] comparison ({6} < {9}).
  H5HF_create_t with_deflate_9 = base;
  auto reset_deflate_9 = absl::MakeCleanup([&]() {
    if (with_deflate_9.pline.nused > 0) {
      H5O_msg_reset(H5O_PLINE_ID, &with_deflate_9.pline);
    }
  });
  unsigned level9 = 9;
  ASSERT_GE(H5Z_append(&with_deflate_9.pline, H5Z_FILTER_DEFLATE,
                       H5Z_FLAG_OPTIONAL, 1, &level9),
            0);
  check_pair(with_deflate_6, with_deflate_9);

  // Equal multi-element pipelines return 0.
  H5HF_create_t with_deflate_6_dup = base;
  auto reset_deflate_6_dup = absl::MakeCleanup([&]() {
    if (with_deflate_6_dup.pline.nused > 0) {
      H5O_msg_reset(H5O_PLINE_ID, &with_deflate_6_dup.pline);
    }
  });
  ASSERT_GE(H5Z_append(&with_deflate_6_dup.pline, H5Z_FILTER_DEFLATE,
                       H5Z_FLAG_OPTIONAL, 1, &level6),
            0);
  EXPECT_EQ(H5HF_cmp_cparam_test(&with_deflate_6, &with_deflate_6_dup), 0);
}

TEST(H5HFtestTest, DtableQueriesIdInspectionTinyAndHugeInfoHelpers) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  // 1. Doubling table inspection + indirect huge & short tiny helpers.
  {
    const H5HF_create_t cparam = MakeHeapCreateParams(
        /*id_len=*/0, /*max_man_size=*/256, /*width=*/4,
        /*start_block_size=*/128, /*max_direct_size=*/512,
        /*max_index=*/18, /*start_root_rows=*/1);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    EXPECT_EQ(H5HF_get_dtable_width_test(fh), 4U);
    const unsigned max_root_rows = H5HF_get_max_root_rows(fh);
    const unsigned max_drows = H5HF_get_dtable_max_drows_test(fh);
    EXPECT_GT(max_root_rows, max_drows);
    EXPECT_GT(max_drows, 1U);

    // Direct rows double after row 1; row_tot_dblock_free excludes overhead.
    EXPECT_EQ(H5HF_get_dblock_size_test(fh, 0), 128U);
    EXPECT_EQ(H5HF_get_dblock_size_test(fh, 1), 128U);
    EXPECT_EQ(H5HF_get_dblock_size_test(fh, 2), 256U);
    EXPECT_EQ(H5HF_get_dblock_size_test(fh, 3), 512U);
    EXPECT_LT(H5HF_get_dblock_free_test(fh, 0), 128U);
    EXPECT_GT(H5HF_get_dblock_free_test(fh, max_drows), 512U);

    EXPECT_EQ(H5HF_get_iblock_max_drows_test(fh, 2),
              H5HF_get_iblock_max_drows_test(fh, 1) + 1);

    size_t tiny_max_len = 0;
    bool tiny_extended = true;
    EXPECT_GE(H5HF_get_tiny_info_test(fh, &tiny_max_len, &tiny_extended), 0);
    EXPECT_GT(tiny_max_len, 0U);
    EXPECT_FALSE(tiny_extended);

    hsize_t next_id = 99;
    bool ids_direct = true;
    EXPECT_GE(H5HF_get_huge_info_test(fh, &next_id, &ids_direct), 0);
    EXPECT_EQ(next_id, 0U);
    EXPECT_FALSE(ids_direct);

    // Insert managed, tiny, and huge objects and verify ID types and offsets.
    size_t id_len = 0;
    ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);
    const std::vector<uint8_t> man1(32, 0x11U);
    const std::vector<uint8_t> man2(48, 0x22U);
    const std::vector<uint8_t> tiny_obj(4, 0x33U);
    const std::vector<uint8_t> huge_obj(300, 0x44U);

    std::vector<uint8_t> id_man1(id_len);
    std::vector<uint8_t> id_man2(id_len);
    std::vector<uint8_t> id_tiny(id_len);
    std::vector<uint8_t> id_huge(id_len);
    ASSERT_GE(H5HF_insert(fh, man1.size(), man1.data(), id_man1.data()), 0);
    ASSERT_GE(H5HF_insert(fh, man2.size(), man2.data(), id_man2.data()), 0);
    ASSERT_GE(
        H5HF_insert(fh, tiny_obj.size(), tiny_obj.data(), id_tiny.data()), 0);
    ASSERT_GE(
        H5HF_insert(fh, huge_obj.size(), huge_obj.data(), id_huge.data()), 0);

    unsigned char id_type = 0xffU;
    EXPECT_GE(H5HF_get_id_type_test(id_man1.data(), &id_type), 0);
    EXPECT_EQ(id_type, H5HF_ID_TYPE_MAN);
    EXPECT_GE(H5HF_get_id_type_test(id_tiny.data(), &id_type), 0);
    EXPECT_EQ(id_type, H5HF_ID_TYPE_TINY);
    EXPECT_GE(H5HF_get_id_type_test(id_huge.data(), &id_type), 0);
    EXPECT_EQ(id_type, H5HF_ID_TYPE_HUGE);

    uint8_t reserved_byte = H5HF_ID_VERS_CURR | H5HF_ID_TYPE_RESERVED;
    EXPECT_GE(H5HF_get_id_type_test(&reserved_byte, &id_type), 0);
    EXPECT_EQ(id_type, H5HF_ID_TYPE_RESERVED);

    hsize_t test_off1 = 0;
    hsize_t pub_off1 = 0;
    hsize_t test_off2 = 0;
    hsize_t pub_off2 = 0;
    EXPECT_GE(H5HF_get_id_off_test(fh, id_man1.data(), &test_off1), 0);
    EXPECT_GE(H5HF_get_obj_off(fh, id_man1.data(), &pub_off1), 0);
    EXPECT_EQ(test_off1, pub_off1);
    EXPECT_GE(H5HF_get_id_off_test(fh, id_man2.data(), &test_off2), 0);
    EXPECT_GE(H5HF_get_obj_off(fh, id_man2.data(), &pub_off2), 0);
    EXPECT_EQ(test_off2, pub_off2);
    EXPECT_EQ(test_off2, test_off1 + man1.size());

    // After inserting one indirect huge object, huge next_id becomes 1; also
    // cover next_id == nullptr branch.
    EXPECT_GE(H5HF_get_huge_info_test(fh, &next_id, &ids_direct), 0);
    EXPECT_EQ(next_id, 1U);
    EXPECT_FALSE(ids_direct);
    ids_direct = true;
    EXPECT_GE(H5HF_get_huge_info_test(fh, nullptr, &ids_direct), 0);
    EXPECT_FALSE(ids_direct);
  }

  // 2. Extended tiny ID (id_len=32) and direct huge ID (id_len=1).
  {
    const H5HF_create_t ext_param = MakeHeapCreateParams(/*id_len=*/32);
    H5HF_t* ext_fh = H5HF_create(f, &ext_param);
    ASSERT_NE(ext_fh, nullptr);
    auto close_ext = absl::MakeCleanup([&]() { H5HF_close(ext_fh); });

    size_t tiny_max_len = 0;
    bool tiny_extended = false;
    EXPECT_GE(H5HF_get_tiny_info_test(ext_fh, &tiny_max_len, &tiny_extended),
              0);
    EXPECT_EQ(tiny_max_len, 30U);
    EXPECT_TRUE(tiny_extended);
  }
  {
    const H5HF_create_t direct_param = MakeHeapCreateParams(/*id_len=*/1);
    H5HF_t* dir_fh = H5HF_create(f, &direct_param);
    ASSERT_NE(dir_fh, nullptr);
    auto close_dir = absl::MakeCleanup([&]() { H5HF_close(dir_fh); });

    hsize_t next_id = 42;
    bool ids_direct = false;
    EXPECT_GE(H5HF_get_huge_info_test(dir_fh, &next_id, &ids_direct), 0);
    EXPECT_EQ(next_id, 0U);
    EXPECT_TRUE(ids_direct);

    ids_direct = false;
    EXPECT_GE(H5HF_get_huge_info_test(dir_fh, nullptr, &ids_direct), 0);
    EXPECT_TRUE(ids_direct);
  }
}

}  // namespace
}  // namespace hdf5
