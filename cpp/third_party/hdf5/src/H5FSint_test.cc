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
// Tests internal free-space manager initialization and flush-dependency
// creation/destruction helpers and error propagation
// (third_party/hdf5/src/H5FSint.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FSint.c

#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5FS_t and H5FS_sinfo_t). Define NDEBUG around internal
// headers so C++ struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5FSINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5FS_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FSpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5FSINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FSINT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

struct TestSection {
  H5FS_section_info_t base{};
};

herr_t FreeTestSection(H5FS_section_info_t* sect) {
  delete reinterpret_cast<TestSection*>(sect);
  return 0;
}

H5FS_section_info_t* DeserializeTestSection(
    const H5FS_section_class_t* /*cls*/, const uint8_t* /*buf*/,
    haddr_t sect_addr, hsize_t sect_size, unsigned* /*des_flags*/) {
  auto* sect = new TestSection();
  sect->base.addr = sect_addr;
  sect->base.size = sect_size;
  sect->base.type = 0;
  sect->base.state = H5FS_SECT_SERIALIZED;
  return &sect->base;
}

const H5FS_section_class_t kSimpleClass = {
    /*type=*/0,
    /*serial_size=*/0,
    /*flags=*/H5FS_CLS_SEPAR_OBJ,
    /*cls_private=*/nullptr,
    /*init_cls=*/nullptr,
    /*term_cls=*/nullptr,
    /*add=*/nullptr,
    /*serialize=*/nullptr,
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

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = 0x20000) {
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

H5FS_create_t MakeFsCreateParams() {
  H5FS_create_t params{};
  params.client = H5FS_CLIENT_FHEAP_ID;
  params.shrink_percent = 80;
  params.expand_percent = 120;
  params.max_sect_addr = 32;
  params.max_sect_size = 0x10000;
  return params;
}

TEST(H5FSintTest, InitIsIdempotent) {
  EXPECT_GE(H5FS_init(), 0);
  EXPECT_GE(H5FS_init(), 0);
}

TEST(H5FSintTest, CreateAndDestroyFlushDependencyDirect) {
  ScopedFile file = CreateInMemoryFile("h5fsint_direct.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5FS_create_t cparam = MakeFsCreateParams();
  const H5FS_section_class_t* classes[] = {&kSimpleClass};

  haddr_t addr1 = HADDR_UNDEF;
  H5FS_t* fs1 = H5FS_create(f, &addr1, &cparam, /*nclasses=*/1, classes,
                            nullptr, /*alignment=*/0, /*threshold=*/0);
  ASSERT_NE(fs1, nullptr);
  absl::Cleanup close_fs1 = [&] {
    if (fs1 != nullptr) {
      H5FS_close(f, fs1);
    }
  };

  haddr_t addr2 = HADDR_UNDEF;
  H5FS_t* fs2 = H5FS_create(f, &addr2, &cparam, /*nclasses=*/1, classes,
                            nullptr, /*alignment=*/0, /*threshold=*/0);
  ASSERT_NE(fs2, nullptr);
  absl::Cleanup close_fs2 = [&] {
    if (fs2 != nullptr) {
      H5FS_close(f, fs2);
    }
  };

  EXPECT_GE(H5FS__create_flush_depend(&fs1->cache_info, &fs2->cache_info), 0);
  EXPECT_GE(H5FS__destroy_flush_depend(&fs1->cache_info, &fs2->cache_info), 0);

  ASSERT_GE(H5FS_close(f, fs2), 0);
  fs2 = nullptr;
  ASSERT_GE(H5FS_close(f, fs1), 0);
  fs1 = nullptr;
}

TEST(H5FSintTest, SwmrHeaderAndSectionInfoCreateAndDestroyFlushDependency) {
  TempFile tmp_file("h5fsint_swmr.h5");
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);

  ScopedFile file(
      H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());
  ASSERT_GE(H5Fstart_swmr_write(file.get()), 0);

  haddr_t fs_addr = HADDR_UNDEF;
  {
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    const H5FS_create_t cparam = MakeFsCreateParams();
    const H5FS_section_class_t* classes[] = {&kSimpleClass};
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, /*nclasses=*/1, classes,
                             nullptr, /*alignment=*/0, /*threshold=*/0);
    ASSERT_NE(fs, nullptr);
    absl::Cleanup close_fs = [&] {
      if (fs != nullptr) {
        H5FS_close(f, fs);
      }
    };

    auto* sect = new TestSection();
    sect->base.addr = 0x100;
    sect->base.size = 64;
    sect->base.type = 0;
    sect->base.state = H5FS_SECT_LIVE;
    ASSERT_GE(H5FS_sect_add(f, fs, &sect->base, /*flags=*/0, nullptr, nullptr),
              0);

    // Closing inserts sinfo into the cache, triggering SWMR AFTER_INSERT and
    // H5FS__create_flush_depend between header (parent) and sinfo (child).
    ASSERT_GE(H5FS_close(f, fs), 0);
    fs = nullptr;
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    // Evicting sinfo exercises BEFORE_EVICT -> H5FS__destroy_flush_depend.
    ASSERT_GE(H5AC_evict(f), 0);

    // Reopening and iterating exercises AFTER_LOAD -> H5FS__create_flush_depend.
    fs = H5FS_open(f, fs_addr, /*nclasses=*/1, classes, nullptr,
                   /*alignment=*/0, /*threshold=*/0);
    ASSERT_NE(fs, nullptr);
    hsize_t seen = 0;
    auto count_cb = [](H5FS_section_info_t* /*s*/, void* op_data) -> herr_t {
      *static_cast<hsize_t*>(op_data) += 1;
      return 0;
    };
    EXPECT_GE(H5FS_sect_iterate(f, fs, count_cb, &seen), 0);
    EXPECT_EQ(seen, 1u);
    ASSERT_GE(H5FS_close(f, fs), 0);
    fs = nullptr;
    ASSERT_GE(H5AC_evict(f), 0);
  }
}

TEST(H5FSintTest, CreateAndDestroyFlushDependPropagateCacheErrors) {
  ScopedFile file = CreateInMemoryFile("h5fsint_err.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5FS_create_t cparam = MakeFsCreateParams();
  const H5FS_section_class_t* classes[] = {&kSimpleClass};

  haddr_t addr1 = HADDR_UNDEF;
  H5FS_t* fs1 = H5FS_create(f, &addr1, &cparam, /*nclasses=*/1, classes,
                            nullptr, /*alignment=*/0, /*threshold=*/0);
  ASSERT_NE(fs1, nullptr);
  absl::Cleanup close_fs1 = [&] {
    if (fs1 != nullptr) {
      H5FS_close(f, fs1);
    }
  };

  haddr_t addr2 = HADDR_UNDEF;
  H5FS_t* fs2 = H5FS_create(f, &addr2, &cparam, /*nclasses=*/1, classes,
                            nullptr, /*alignment=*/0, /*threshold=*/0);
  ASSERT_NE(fs2, nullptr);
  // Close fs2 so it stays resident in cache but is neither pinned nor protected.
  ASSERT_GE(H5FS_close(f, fs2), 0);

  ScopedErrorSilence silence;
  // Self-dependency fails in H5C_create_flush_dependency.
  EXPECT_LT(H5FS__create_flush_depend(&fs1->cache_info, &fs1->cache_info), 0);
  // Unpinned/unprotected parent fails in H5C_create_flush_dependency and
  // H5C_destroy_flush_dependency.
  EXPECT_LT(H5FS__create_flush_depend(&fs2->cache_info, &fs1->cache_info), 0);
  EXPECT_LT(H5FS__destroy_flush_depend(&fs1->cache_info, &fs2->cache_info), 0);

  ASSERT_GE(H5FS_close(f, fs1), 0);
  fs1 = nullptr;
}

}  // namespace
}  // namespace hdf5
