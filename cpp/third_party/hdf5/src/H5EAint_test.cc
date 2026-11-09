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
// Tests internal extensible array flush-dependency creation/destruction helpers
// and error propagation (third_party/hdf5/src/H5EAint.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5EAint.c

#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5EA_hdr_t / H5EA_iblock_t / H5EA_sblock_t). Define NDEBUG
// around internal headers so struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5EAINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5EA_FRIEND
#define H5EA_TESTING
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5EApkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5EAINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5EAINT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

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

class ScopedEa {
 public:
  ScopedEa() = default;
  explicit ScopedEa(H5EA_t* ea) : ea_(ea) {}
  ~ScopedEa() { reset(); }

  ScopedEa(const ScopedEa&) = delete;
  ScopedEa& operator=(const ScopedEa&) = delete;

  void reset(H5EA_t* next = nullptr) {
    if (ea_ != nullptr) {
      H5EA_close(ea_);
    }
    ea_ = next;
  }

  H5EA_t* get() const { return ea_; }

 private:
  H5EA_t* ea_ = nullptr;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5EA_create_t MakeTestCreateParams() {
  H5EA_create_t cparam{};
  cparam.cls = H5EA_CLS_TEST;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_nelmts_bits = 10;
  cparam.idx_blk_elmts = 4;
  cparam.data_blk_min_elmts = 4;
  cparam.sup_blk_min_data_ptrs = 4;
  cparam.max_dblk_page_nelmts_bits = 4;
  return cparam;
}

ScopedFile CreateSwmrTempFile(const TempFile& tmp_file) {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST) <
      0) {
    return ScopedFile();
  }
  ScopedFile file(
      H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  if (!file.valid()) {
    return ScopedFile();
  }
  if (H5Fstart_swmr_write(file.get()) < 0) {
    return ScopedFile();
  }
  return file;
}

TEST(H5EAintTest, CreateAndDestroyFlushDependDirectAndViaSwmrArray) {
  // 1. Direct flush dependency creation and destruction between cached entries.
  {
    ScopedFile file = CreateInMemoryFile("h5eaint_direct.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    const H5EA_create_t cparam = MakeTestCreateParams();
    haddr_t hdr_addr1 = H5EA__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(hdr_addr1));
    haddr_t hdr_addr2 = H5EA__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(hdr_addr2));

    H5EA_hdr_t* hdr1 = H5EA__hdr_protect(f, hdr_addr1, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr1, nullptr);
    H5EA_hdr_t* hdr2 = H5EA__hdr_protect(f, hdr_addr2, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr2, nullptr);

    EXPECT_GE(H5EA__create_flush_depend(&hdr1->cache_info, &hdr2->cache_info), 0);
    EXPECT_GE(H5EA__destroy_flush_depend(&hdr1->cache_info, &hdr2->cache_info),
              0);

    ASSERT_GE(H5EA__hdr_unprotect(hdr2, H5AC__NO_FLAGS_SET), 0);
    ASSERT_GE(H5EA__hdr_unprotect(hdr1, H5AC__NO_FLAGS_SET), 0);
  }

  // 2. SWMR array growth/flush exercises iblock, sblock, dblock, and dblk_page
  // flush dependency creation and destruction.
  TempFile tmp_file("h5eaint_swmr.h5");
  ScopedFile file = CreateSwmrTempFile(tmp_file);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5EA_create_t cparam = MakeTestCreateParams();
  ScopedEa ea(H5EA_create(f, &cparam, nullptr));
  ASSERT_NE(ea.get(), nullptr);

  // Cover index block, direct dblock, unpaged sblock dblock, and paged dblock.
  for (hsize_t idx : {hsize_t{0}, hsize_t{4}, hsize_t{64}, hsize_t{128}}) {
    uint64_t val = idx + 1;
    ASSERT_GE(H5EA_set(ea.get(), idx, &val), 0);
  }
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  for (hsize_t idx : {hsize_t{0}, hsize_t{4}, hsize_t{64}, hsize_t{128}}) {
    uint64_t out = 0;
    ASSERT_GE(H5EA_get(ea.get(), idx, &out), 0);
    EXPECT_EQ(out, idx + 1);
  }
}

TEST(H5EAintTest, CreateAndDestroyFlushDependPropagateCacheErrors) {
  ScopedFile file = CreateInMemoryFile("h5eaint_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5EA_create_t cparam = MakeTestCreateParams();
  haddr_t hdr_addr1 = H5EA__hdr_create(f, &cparam, nullptr);
  ASSERT_TRUE(H5_addr_defined(hdr_addr1));
  haddr_t hdr_addr2 = H5EA__hdr_create(f, &cparam, nullptr);
  ASSERT_TRUE(H5_addr_defined(hdr_addr2));

  // Protect hdr1 cleanly; keep hdr2 resident in cache but unprotected & unpinned.
  H5EA_hdr_t* hdr1 = H5EA__hdr_protect(f, hdr_addr1, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr1, nullptr);
  absl::Cleanup unprot_hdr1 = [&] {
    if (hdr1 != nullptr) {
      H5EA__hdr_unprotect(hdr1, H5AC__NO_FLAGS_SET);
    }
  };

  H5EA_hdr_t* hdr2 = H5EA__hdr_protect(f, hdr_addr2, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr2, nullptr);
  ASSERT_GE(H5EA__hdr_unprotect(hdr2, H5AC__NO_FLAGS_SET), 0);

  ScopedErrorSilence silence;
  // Self-dependency fails cleanly in H5C_create_flush_dependency.
  EXPECT_LT(H5EA__create_flush_depend(&hdr1->cache_info, &hdr1->cache_info), 0);

  // Unpinned parent entry fails cleanly in H5C_destroy_flush_dependency.
  EXPECT_LT(H5EA__destroy_flush_depend(&hdr2->cache_info, &hdr1->cache_info), 0);
}

}  // namespace
}  // namespace hdf5
