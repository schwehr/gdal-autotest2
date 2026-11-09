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
// Tests free-space manager creation parameter retrieval and comparison helpers
// (third_party/hdf5/src/H5FStest.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FStest.c

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with `-DNDEBUG`, which omits debug/stats fields from
// `H5C_cache_entry_t` (`H5AC_info_t`), the first member of `H5FS_t`. Define
// `NDEBUG` around the prelude and internal headers so struct offsets match.
#ifndef NDEBUG
#define H5FSTEST_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5FS_FRIEND
#define H5FS_TESTING
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FSpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5FSTEST_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FSTEST_TEST_RESTORE_UNDEF_NDEBUG
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

const H5FS_section_class_t kMinimalClass = {
    /*type=*/0,
    /*serial_size=*/0,
    /*flags=*/0,
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
    /*free=*/nullptr,
    /*valid=*/nullptr,
    /*split=*/nullptr,
    /*debug=*/nullptr,
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
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

TEST(H5FStestTest, GetCreationParametersNonPersistentAndReopened) {
  constexpr H5FS_create_t kNonPersistParams{
      .client = H5FS_CLIENT_FHEAP_ID,
      .shrink_percent = 25,
      .expand_percent = 150,
      .max_sect_addr = 40,
      .max_sect_size = 131072,
  };
  constexpr H5FS_create_t kPersistParams{
      .client = H5FS_CLIENT_FILE_ID,
      .shrink_percent = 40,
      .expand_percent = 200,
      .max_sect_addr = 48,
      .max_sect_size = 262144,
  };

  haddr_t persist_addr = HADDR_UNDEF;
  std::vector<uint8_t> image;
  const H5FS_section_class_t* classes[] = {&kMinimalClass};

  // 1. Verify H5FS__get_cparam_test on non-persistent and persistent managers.
  {
    ScopedFile file = CreateInMemoryFile("h5fstest_get.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(nullptr, f);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    ScopedFspace non_persist(
        f, H5FS_create(f, /*fs_addr=*/nullptr, &kNonPersistParams,
                       /*nclasses=*/0, /*classes=*/nullptr,
                       /*cls_init_udata=*/nullptr, /*alignment=*/0,
                       /*threshold=*/0));
    ASSERT_NE(nullptr, non_persist.get());

    H5FS_create_t actual{};
    std::memset(&actual, 0x7f, sizeof(actual));
    ASSERT_GE(H5FS__get_cparam_test(non_persist.get(), &actual), 0);
    EXPECT_EQ(kNonPersistParams.client, actual.client);
    EXPECT_EQ(kNonPersistParams.shrink_percent, actual.shrink_percent);
    EXPECT_EQ(kNonPersistParams.expand_percent, actual.expand_percent);
    EXPECT_EQ(kNonPersistParams.max_sect_addr, actual.max_sect_addr);
    EXPECT_EQ(kNonPersistParams.max_sect_size, actual.max_sect_size);
    EXPECT_EQ(0, H5FS__cmp_cparam_test(&kNonPersistParams, &actual));

    ScopedFspace persist(
        f, H5FS_create(f, &persist_addr, &kPersistParams, /*nclasses=*/1,
                       classes, /*cls_init_udata=*/nullptr, /*alignment=*/0,
                       /*threshold=*/0));
    ASSERT_NE(nullptr, persist.get());
    ASSERT_NE(HADDR_UNDEF, persist_addr);

    non_persist.reset();
    persist.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    image = SnapshotFileImage(file.get());
    ASSERT_FALSE(image.empty());
  }

  // 2. Reopen serialized image and confirm persisted creation parameters match.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("h5fstest_reopen.h5", H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(reopened.valid());
  H5F_t* rf = GetH5File(reopened.get());
  ASSERT_NE(nullptr, rf);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  ScopedFspace reopened_fs(
      rf, H5FS_open(rf, persist_addr, /*nclasses=*/1, classes,
                    /*cls_init_udata=*/nullptr, /*alignment=*/0,
                    /*threshold=*/0));
  ASSERT_NE(nullptr, reopened_fs.get());

  H5FS_create_t reopened_params{};
  std::memset(&reopened_params, 0x33, sizeof(reopened_params));
  ASSERT_GE(H5FS__get_cparam_test(reopened_fs.get(), &reopened_params), 0);
  EXPECT_EQ(0, H5FS__cmp_cparam_test(&kPersistParams, &reopened_params));
}

TEST(H5FStestTest, CmpCreationParametersCoversAllOrderingBranches) {
  ASSERT_GE(H5open(), 0);

  struct CmpCase {
    std::string_view label;
    H5FS_create_t lhs;
    H5FS_create_t rhs;
    int expected;
  };

  constexpr H5FS_create_t kRef{
      .client = H5FS_CLIENT_FILE_ID,
      .shrink_percent = 50,
      .expand_percent = 150,
      .max_sect_addr = 32,
      .max_sect_size = 4096,
  };

  constexpr auto kCases = std::to_array<CmpCase>({
      {
          .label = "equal",
          .lhs = kRef,
          .rhs = kRef,
          .expected = 0,
      },
      {
          .label = "client_lt",
          .lhs = {H5FS_CLIENT_FHEAP_ID, 99, 999, 64, 999999},
          .rhs = kRef,
          .expected = -1,
      },
      {
          .label = "client_gt",
          .lhs = {H5FS_NUM_CLIENT_ID, 1, 2, 1, 1},
          .rhs = kRef,
          .expected = 1,
      },
      {
          .label = "shrink_percent_lt",
          .lhs = {H5FS_CLIENT_FILE_ID, 49, 999, 64, 999999},
          .rhs = kRef,
          .expected = -1,
      },
      {
          .label = "shrink_percent_gt",
          .lhs = {H5FS_CLIENT_FILE_ID, 51, 1, 1, 1},
          .rhs = kRef,
          .expected = 1,
      },
      {
          .label = "expand_percent_lt",
          .lhs = {H5FS_CLIENT_FILE_ID, 50, 149, 64, 999999},
          .rhs = kRef,
          .expected = -1,
      },
      {
          .label = "expand_percent_gt",
          .lhs = {H5FS_CLIENT_FILE_ID, 50, 151, 1, 1},
          .rhs = kRef,
          .expected = 1,
      },
      {
          .label = "max_sect_size_lt",
          .lhs = {H5FS_CLIENT_FILE_ID, 50, 150, 64, 4095},
          .rhs = kRef,
          .expected = -1,
      },
      {
          .label = "max_sect_size_gt",
          .lhs = {H5FS_CLIENT_FILE_ID, 50, 150, 1, 4097},
          .rhs = kRef,
          .expected = 1,
      },
      {
          .label = "max_sect_addr_lt",
          .lhs = {H5FS_CLIENT_FILE_ID, 50, 150, 31, 4096},
          .rhs = kRef,
          .expected = -1,
      },
      {
          .label = "max_sect_addr_gt",
          .lhs = {H5FS_CLIENT_FILE_ID, 50, 150, 33, 4096},
          .rhs = kRef,
          .expected = 1,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.label);
    EXPECT_EQ(tc.expected, H5FS__cmp_cparam_test(&tc.lhs, &tc.rhs));
  }
}

}  // namespace
}  // namespace hdf5
