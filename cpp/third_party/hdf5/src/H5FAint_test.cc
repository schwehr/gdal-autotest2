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
// Tests internal fixed array flush-dependency creation/destruction helpers and
// error propagation (third_party/hdf5/src/H5FAint.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FAint.c

#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5FA_hdr_t / H5FA_dblock_t). Define NDEBUG around internal
// headers so struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5FAINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5FA_FRIEND
#define H5FA_TESTING
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FApkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5FAINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FAINT_TEST_RESTORE_UNDEF_NDEBUG
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

class ScopedFa {
 public:
  ScopedFa() = default;
  explicit ScopedFa(H5FA_t* fa) : fa_(fa) {}
  ~ScopedFa() { reset(); }

  ScopedFa(const ScopedFa&) = delete;
  ScopedFa& operator=(const ScopedFa&) = delete;

  void reset(H5FA_t* next = nullptr) {
    if (fa_ != nullptr) {
      H5FA_close(fa_);
    }
    fa_ = next;
  }

  H5FA_t* get() const { return fa_; }

 private:
  H5FA_t* fa_ = nullptr;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5FA_create_t MakeTestCreateParams(hsize_t nelmts = 32) {
  H5FA_create_t cparam{};
  cparam.cls = H5FA_CLS_TEST;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_dblk_page_nelmts_bits = 4;  // 16 elements per page.
  cparam.nelmts = nelmts;
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

TEST(H5FAintTest, CreateAndDestroyFlushDependDirectAndViaSwmrArray) {
  // 1. Direct flush dependency creation and destruction between cached entries.
  {
    ScopedFile file = CreateInMemoryFile("h5faint_direct.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    const H5FA_create_t cparam = MakeTestCreateParams(/*nelmts=*/8);
    haddr_t hdr_addr1 = H5FA__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(hdr_addr1));
    haddr_t hdr_addr2 = H5FA__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(hdr_addr2));

    H5FA_hdr_t* hdr1 =
        H5FA__hdr_protect(f, hdr_addr1, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr1, nullptr);
    absl::Cleanup unprot_hdr1 = [&] {
      if (hdr1 != nullptr) {
        H5FA__hdr_unprotect(hdr1, H5AC__NO_FLAGS_SET);
      }
    };

    H5FA_hdr_t* hdr2 =
        H5FA__hdr_protect(f, hdr_addr2, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr2, nullptr);
    absl::Cleanup unprot_hdr2 = [&] {
      if (hdr2 != nullptr) {
        H5FA__hdr_unprotect(hdr2, H5AC__NO_FLAGS_SET);
      }
    };

    EXPECT_GE(H5FA__create_flush_depend(&hdr1->cache_info, &hdr2->cache_info),
              0);
    EXPECT_GE(H5FA__destroy_flush_depend(&hdr1->cache_info, &hdr2->cache_info),
              0);

    ASSERT_GE(H5FA__hdr_unprotect(hdr2, H5AC__NO_FLAGS_SET), 0);
    hdr2 = nullptr;
    ASSERT_GE(H5FA__hdr_unprotect(hdr1, H5AC__NO_FLAGS_SET), 0);
    hdr1 = nullptr;
  }

  // 2. SWMR fixed array write/flush exercises dblock flush dependency creation
  // and destruction (via H5FA__cache_dblock_notify) on unpaged and paged
  // arrays.
  TempFile tmp_file("h5faint_swmr.h5");
  ScopedFile file = CreateSwmrTempFile(tmp_file);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  // Unpaged SWMR array (nelmts=8 <= 16 elements per page).
  {
    const H5FA_create_t unpaged_cparam = MakeTestCreateParams(/*nelmts=*/8);
    ScopedFa fa(H5FA_create(f, &unpaged_cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);

    for (hsize_t idx : {hsize_t{0}, hsize_t{7}}) {
      uint64_t val = idx + 100;
      ASSERT_GE(H5FA_set(fa.get(), idx, &val), 0);
    }
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    for (hsize_t idx : {hsize_t{0}, hsize_t{7}}) {
      uint64_t out = 0;
      ASSERT_GE(H5FA_get(fa.get(), idx, &out), 0);
      EXPECT_EQ(out, idx + 100);
    }
  }

  // Paged SWMR array (nelmts=32 -> 2 pages of 16 elements).
  {
    const H5FA_create_t paged_cparam = MakeTestCreateParams(/*nelmts=*/32);
    ScopedFa fa(H5FA_create(f, &paged_cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);

    for (hsize_t idx : {hsize_t{0}, hsize_t{15}, hsize_t{16}, hsize_t{31}}) {
      uint64_t val = idx + 200;
      ASSERT_GE(H5FA_set(fa.get(), idx, &val), 0);
    }
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    for (hsize_t idx : {hsize_t{0}, hsize_t{15}, hsize_t{16}, hsize_t{31}}) {
      uint64_t out = 0;
      ASSERT_GE(H5FA_get(fa.get(), idx, &out), 0);
      EXPECT_EQ(out, idx + 200);
    }
  }
}

TEST(H5FAintTest, CreateAndDestroyFlushDependPropagateCacheErrors) {
  ScopedFile file = CreateInMemoryFile("h5faint_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5FA_create_t cparam = MakeTestCreateParams(/*nelmts=*/8);
  haddr_t hdr_addr1 = H5FA__hdr_create(f, &cparam, nullptr);
  ASSERT_TRUE(H5_addr_defined(hdr_addr1));
  haddr_t hdr_addr2 = H5FA__hdr_create(f, &cparam, nullptr);
  ASSERT_TRUE(H5_addr_defined(hdr_addr2));

  // Keep hdr1 protected; leave hdr2 resident in cache but unprotected/unpinned.
  H5FA_hdr_t* hdr1 =
      H5FA__hdr_protect(f, hdr_addr1, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr1, nullptr);
  absl::Cleanup unprot_hdr1 = [&] {
    if (hdr1 != nullptr) {
      H5FA__hdr_unprotect(hdr1, H5AC__NO_FLAGS_SET);
    }
  };

  H5FA_hdr_t* hdr2 =
      H5FA__hdr_protect(f, hdr_addr2, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr2, nullptr);
  ASSERT_GE(H5FA__hdr_unprotect(hdr2, H5AC__NO_FLAGS_SET), 0);

  ScopedErrorSilence silence;
  // Self-dependency fails in H5C_create_flush_dependency and exercises the
  // H5E_CANTDEPEND HGOTO_ERROR path in H5FA__create_flush_depend.
  EXPECT_LT(H5FA__create_flush_depend(&hdr1->cache_info, &hdr1->cache_info), 0);

  // Unpinned parent fails in H5C_create_flush_dependency and
  // H5C_destroy_flush_dependency, exercising the error paths in both helpers.
  EXPECT_LT(H5FA__create_flush_depend(&hdr2->cache_info, &hdr1->cache_info), 0);
  EXPECT_LT(H5FA__destroy_flush_depend(&hdr2->cache_info, &hdr1->cache_info),
            0);
}

}  // namespace
}  // namespace hdf5
