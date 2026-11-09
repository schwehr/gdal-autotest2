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
// Tests internal H5F space allocation, freeing, and end-of-file extension
// wrappers (third_party/hdf5/src/H5Fspace.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Fspace.c

#include <cstring>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits #ifndef NDEBUG fields from
// H5C_cache_entry_t (H5AC_info_t) inside H5F_super_t. Define NDEBUG around
// hdf5_internal_prelude.h and internal headers so struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5FSPACE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#define try hdf5_try_param
extern "C" {
#define H5F_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FDprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5FSPACE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FSPACE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  ScopedApiContext() {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(H5AC__SUPERBLOCK_TAG, &prev_tag_);
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

 private:
  H5CX_node_t node_ = {};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

haddr_t FailingGetEoa(const H5FD_t* /*file*/, H5FD_mem_t /*type*/) {
  return HADDR_UNDEF;
}

TEST(H5FspaceTest, AllocExtendAndFreeSucceedWithAndWithoutTmpSpace) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_alignment(fapl.get(), /*threshold=*/1, /*alignment=*/64), 0);

  ScopedFile file(
      H5Fcreate("space_ok.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;

  // 1. Allocate space with use_tmp_space enabled (default).
  haddr_t frag_addr = HADDR_UNDEF;
  hsize_t frag_size = 0;
  const haddr_t addr1 =
      H5F__alloc(f, H5FD_MEM_DRAW, /*size=*/128, &frag_addr, &frag_size);
  ASSERT_NE(addr1, HADDR_UNDEF);
  EXPECT_EQ(addr1 % 64, 0U);

  // 2. Extend exactly at current EOA, then verify extending non-EOA returns 0.
  const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DRAW);
  ASSERT_NE(eoa, HADDR_UNDEF);
  EXPECT_GT(H5F__try_extend(f, H5FD_MEM_DRAW, eoa, /*extra_requested=*/32), 0);
  EXPECT_EQ(H5F__try_extend(f, H5FD_MEM_DRAW, addr1, /*extra_requested=*/32),
            0);

  // 3. Free previously allocated/extended space.
  EXPECT_GE(H5F__free(f, H5FD_MEM_DRAW, addr1, /*size=*/128 + 32), 0);

  // 4. Allocate with use_tmp_space disabled, restoring state afterward.
  const bool orig_use_tmp = f->shared->use_tmp_space;
  absl::Cleanup restore_tmp = [&]() {
    f->shared->use_tmp_space = orig_use_tmp;
  };
  f->shared->use_tmp_space = false;

  const haddr_t addr2 =
      H5F__alloc(f, H5FD_MEM_DRAW, /*size=*/64, &frag_addr, &frag_size);
  ASSERT_NE(addr2, HADDR_UNDEF);
  EXPECT_GE(H5F__free(f, H5FD_MEM_DRAW, addr2, /*size=*/64), 0);
}

TEST(H5FspaceTest, AllocDetectsTmpSpaceOverlapGetEoaAndDriverFailures) {
  ScopedFile file = CreateInMemoryFile("space_alloc_err.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ScopedErrorSilence silence;

  haddr_t frag_addr = HADDR_UNDEF;
  hsize_t frag_size = 0;

  // 1. Fail get_eoa via a per-file stack-local VFD vtable override.
  {
    const H5FD_class_t* orig_cls = f->shared->lf->cls;
    H5FD_class_t failing_cls = *orig_cls;
    failing_cls.get_eoa = FailingGetEoa;
    absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };
    f->shared->lf->cls = &failing_cls;

    EXPECT_EQ(H5F__alloc(f, H5FD_MEM_DRAW, /*size=*/16, &frag_addr, &frag_size),
              HADDR_UNDEF);
  }

  // 2. Request space overflowing into temporary allocation address space.
  {
    const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DRAW);
    ASSERT_NE(eoa, HADDR_UNDEF);
    const haddr_t orig_tmp_addr = f->shared->tmp_addr;
    absl::Cleanup restore_tmp_addr = [&]() {
      f->shared->tmp_addr = orig_tmp_addr;
    };
    f->shared->tmp_addr = eoa + 16;

    EXPECT_EQ(H5F__alloc(f, H5FD_MEM_DRAW, /*size=*/64, &frag_addr, &frag_size),
              HADDR_UNDEF);
  }

  // 3. Request exceeding maxaddr forces H5FD_alloc failure with tmp check off.
  {
    const bool orig_use_tmp = f->shared->use_tmp_space;
    absl::Cleanup restore_use_tmp = [&]() {
      f->shared->use_tmp_space = orig_use_tmp;
    };
    f->shared->use_tmp_space = false;

    EXPECT_EQ(H5F__alloc(f, H5FD_MEM_DRAW, /*size=*/HADDR_MAX - 10, &frag_addr,
                         &frag_size),
              HADDR_UNDEF);
  }
}

TEST(H5FspaceTest, FreeAndTryExtendHandleDriverFailuresCleanly) {
  ScopedFile file = CreateInMemoryFile("space_free_extend_err.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ScopedErrorSilence silence;

  // Invalid address triggers H5FD_free failure in H5F__free.
  EXPECT_LT(H5F__free(f, H5FD_MEM_DRAW, HADDR_UNDEF, /*size=*/16), 0);

  // Extending at EOA with overflowing size fails in H5FD_try_extend.
  const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DRAW);
  ASSERT_NE(eoa, HADDR_UNDEF);
  EXPECT_LT(H5F__try_extend(f, H5FD_MEM_DRAW, eoa, HADDR_MAX), 0);
}

}  // namespace
}  // namespace hdf5
