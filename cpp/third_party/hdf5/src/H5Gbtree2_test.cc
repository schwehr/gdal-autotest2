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
// Tests v2 B-tree callbacks for dense group link indexing (name hash index,
// creation order index, encode/decode serialization, lookup3 hash collisions,
// and debug/error paths) for third_party/hdf5/src/H5Gbtree2.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Gbtree2.c

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from cache
// entries in internal headers. Match those struct layouts here.
#ifndef NDEBUG
#define H5GBTREE2_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

// H5B2private.h (pulled via H5Gpkg.h) uses `try` as a C parameter name, which
// collides with the C++ keyword and Google3's `-fno-exceptions` `try` macro.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_param_try
extern "C" {
#define H5G_FRIEND
#include "third_party/hdf5/src/H5B2private.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Gpkg.h"
#include "third_party/hdf5/src/H5HFprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5GBTREE2_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5GBTREE2_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

// RAII wrapper for pushing/popping an HDF5 API context.
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

ScopedPlist MakeDenseLinkGroupCreatePlist() {
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  if (!gcpl.valid()) {
    return ScopedPlist();
  }
  if (H5Pset_link_phase_change(gcpl.get(), 0, 0) < 0 ||
      H5Pset_link_creation_order(
          gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
    gcpl.reset();
  }
  return gcpl;
}

TEST(H5Gbtree2Test, ClassDescriptorsMatchExpectedDefinitions) {
  EXPECT_EQ(H5G_BT2_NAME[0].id, H5B2_GRP_DENSE_NAME_ID);
  EXPECT_STREQ(H5G_BT2_NAME[0].name, "H5B2_GRP_DENSE_NAME_ID");
  EXPECT_EQ(H5G_BT2_NAME[0].nrec_size, sizeof(H5G_dense_bt2_name_rec_t));
  EXPECT_EQ(H5G_BT2_NAME[0].crt_context, nullptr);
  EXPECT_EQ(H5G_BT2_NAME[0].dst_context, nullptr);
  EXPECT_NE(H5G_BT2_NAME[0].store, nullptr);
  EXPECT_NE(H5G_BT2_NAME[0].compare, nullptr);
  EXPECT_NE(H5G_BT2_NAME[0].encode, nullptr);
  EXPECT_NE(H5G_BT2_NAME[0].decode, nullptr);
  EXPECT_NE(H5G_BT2_NAME[0].debug, nullptr);

  EXPECT_EQ(H5G_BT2_CORDER[0].id, H5B2_GRP_DENSE_CORDER_ID);
  EXPECT_STREQ(H5G_BT2_CORDER[0].name, "H5B2_GRP_DENSE_CORDER_ID");
  EXPECT_EQ(H5G_BT2_CORDER[0].nrec_size, sizeof(H5G_dense_bt2_corder_rec_t));
  EXPECT_EQ(H5G_BT2_CORDER[0].crt_context, nullptr);
  EXPECT_EQ(H5G_BT2_CORDER[0].dst_context, nullptr);
  EXPECT_NE(H5G_BT2_CORDER[0].store, nullptr);
  EXPECT_NE(H5G_BT2_CORDER[0].compare, nullptr);
  EXPECT_NE(H5G_BT2_CORDER[0].encode, nullptr);
  EXPECT_NE(H5G_BT2_CORDER[0].decode, nullptr);
  EXPECT_NE(H5G_BT2_CORDER[0].debug, nullptr);
}

TEST(H5Gbtree2Test, DenseNameAndCorderOperationsAndReopen) {
  ScopedFile file = CreateInMemoryFile("h5gbtree2_dense.h5");
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl = MakeDenseLinkGroupCreatePlist();
  ASSERT_TRUE(gcpl.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/dense_grp", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // 1. Insert 100 soft links so both H5G_BT2_NAME and H5G_BT2_CORDER span
  //    multiple 512-byte v2 B-tree nodes (exercising store and compare).
  constexpr int kNumLinks = 100;
  for (int i = 0; i < kNumLinks; ++i) {
    std::string link_name = absl::StrFormat("link_%03d", i);
    std::string target = absl::StrFormat("/target_%03d", i);
    ASSERT_GE(H5Lcreate_soft(target.c_str(), grp.get(), link_name.c_str(),
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // 2. Probe missing names (exercising hash < and > comparisons) and delete
  //    entries via both name and creation-order indices.
  EXPECT_EQ(H5Lexists(grp.get(), "missing_low_hash", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(grp.get(), "zzzz_missing_high_hash", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(grp.get(), "link_010", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete_by_idx(grp.get(), ".", H5_INDEX_CRT_ORDER, H5_ITER_DEC, 0,
                             H5P_DEFAULT),
            0);
  grp.reset();

  // 3. Flush to force encode callbacks, then reopen from file image to force
  //    decode callbacks on both B-trees.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
            image_size);
  file.reset();

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("reopen_dense_links.h5", H5F_ACC_RDONLY,
                              fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedGroup reopened_grp(H5Gopen2(reopened.get(), "/dense_grp", H5P_DEFAULT));
  ASSERT_TRUE(reopened_grp.valid());

  EXPECT_EQ(H5Lexists(reopened_grp.get(), "link_000", H5P_DEFAULT), 1);
  EXPECT_EQ(H5Lexists(reopened_grp.get(), "link_010", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(reopened_grp.get(), "link_099", H5P_DEFAULT), 0);

  char val_buf[64] = {};
  ASSERT_GE(H5Lget_val(reopened_grp.get(), "link_042", val_buf, sizeof(val_buf),
                       H5P_DEFAULT),
            0);
  EXPECT_STREQ(val_buf, "/target_042");

  // 4. Native iteration on both indices walks decoded B-tree records directly.
  auto collect_cb = [](hid_t /*group*/, const char* name,
                       const H5L_info2_t* /*linfo*/, void* op_data) -> herr_t {
    static_cast<std::vector<std::string>*>(op_data)->emplace_back(name);
    return 0;
  };

  std::vector<std::string> by_name;
  hsize_t idx = 0;
  ASSERT_GE(H5Literate2(reopened_grp.get(), H5_INDEX_NAME, H5_ITER_NATIVE, &idx,
                        collect_cb, &by_name),
            0);
  EXPECT_EQ(by_name.size(), kNumLinks - 2);

  std::vector<std::string> by_corder;
  idx = 0;
  ASSERT_GE(H5Literate2(reopened_grp.get(), H5_INDEX_CRT_ORDER, H5_ITER_NATIVE,
                        &idx, collect_cb, &by_corder),
            0);
  EXPECT_EQ(by_corder.size(), kNumLinks - 2);
}

TEST(H5Gbtree2Test, Lookup3HashCollisionExercisesFhNameCmpMismatch) {
  // "k_2499" and "k_87023" collide under H5_checksum_lookup3(..., 0).
  constexpr char kSmallName[] = "k_2499";
  constexpr char kLargeName[] = "k_87023";
  ASSERT_LT(std::strcmp(kSmallName, kLargeName), 0);
  ASSERT_EQ(H5_checksum_lookup3(kSmallName, std::strlen(kSmallName), 0),
            H5_checksum_lookup3(kLargeName, std::strlen(kLargeName), 0));

  ScopedFile file = CreateInMemoryFile("h5gbtree2_collide.h5");
  ASSERT_TRUE(file.valid());
  ScopedPlist gcpl = MakeDenseLinkGroupCreatePlist();
  ASSERT_TRUE(gcpl.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/collide", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // 1. Store kLargeName first; probing kSmallName hits udata->cmp < 0.
  ASSERT_GE(H5Lcreate_soft("/large_target", grp.get(), kLargeName, H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Lexists(grp.get(), kSmallName, H5P_DEFAULT), 0);

  // 2. Store kSmallName as well; both coexist with identical name hashes.
  ASSERT_GE(H5Lcreate_soft("/small_target", grp.get(), kSmallName, H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Lexists(grp.get(), kSmallName, H5P_DEFAULT), 1);
  EXPECT_EQ(H5Lexists(grp.get(), kLargeName, H5P_DEFAULT), 1);

  char buf[32] = {};
  ASSERT_GE(
      H5Lget_val(grp.get(), kSmallName, buf, sizeof(buf), H5P_DEFAULT), 0);
  EXPECT_STREQ(buf, "/small_target");

  std::memset(buf, 0, sizeof(buf));
  ASSERT_GE(
      H5Lget_val(grp.get(), kLargeName, buf, sizeof(buf), H5P_DEFAULT), 0);
  EXPECT_STREQ(buf, "/large_target");

  // 3. Remove kLargeName and probe kLargeName vs kSmallName to hit cmp > 0.
  ASSERT_GE(H5Ldelete(grp.get(), kLargeName, H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(grp.get(), kLargeName, H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(grp.get(), kSmallName, H5P_DEFAULT), 1);
}

TEST(H5Gbtree2Test, DirectCallbacksRoundTripDebugAndErrorPaths) {
  // 1. Direct encode/decode/store/compare/debug checks for both B-tree classes.
  {
    H5G_bt2_ud_ins_t ins{};
    ins.common.name_hash = 0x12345678U;
    ins.common.corder = 987654321LL;
    for (size_t i = 0; i < H5G_DENSE_FHEAP_ID_LEN; ++i) {
      ins.id[i] = static_cast<uint8_t>(0x10 + i);
    }

    H5G_dense_bt2_name_rec_t name_rec{};
    ASSERT_GE(H5G_BT2_NAME[0].store(&name_rec, &ins), 0);
    EXPECT_EQ(name_rec.hash, 0x12345678U);
    EXPECT_EQ(std::memcmp(name_rec.id, ins.id, H5G_DENSE_FHEAP_ID_LEN), 0);

    std::array<uint8_t, 4 + H5G_DENSE_FHEAP_ID_LEN> raw_name{};
    ASSERT_GE(H5G_BT2_NAME[0].encode(raw_name.data(), &name_rec, nullptr), 0);
    H5G_dense_bt2_name_rec_t decoded_name{};
    ASSERT_GE(H5G_BT2_NAME[0].decode(raw_name.data(), &decoded_name, nullptr),
              0);
    EXPECT_EQ(decoded_name.hash, name_rec.hash);
    EXPECT_EQ(std::memcmp(decoded_name.id, name_rec.id, H5G_DENSE_FHEAP_ID_LEN),
              0);

    H5G_dense_bt2_corder_rec_t corder_rec{};
    ASSERT_GE(H5G_BT2_CORDER[0].store(&corder_rec, &ins), 0);
    EXPECT_EQ(corder_rec.corder, 987654321LL);
    EXPECT_EQ(std::memcmp(corder_rec.id, ins.id, H5G_DENSE_FHEAP_ID_LEN), 0);

    int cmp_res = 0;
    H5G_bt2_ud_common_t cmp_ud{};
    cmp_ud.corder = 987654320LL;
    ASSERT_GE(H5G_BT2_CORDER[0].compare(&cmp_ud, &corder_rec, &cmp_res), 0);
    EXPECT_LT(cmp_res, 0);
    cmp_ud.corder = 987654322LL;
    ASSERT_GE(H5G_BT2_CORDER[0].compare(&cmp_ud, &corder_rec, &cmp_res), 0);
    EXPECT_GT(cmp_res, 0);
    cmp_ud.corder = 987654321LL;
    ASSERT_GE(H5G_BT2_CORDER[0].compare(&cmp_ud, &corder_rec, &cmp_res), 0);
    EXPECT_EQ(cmp_res, 0);

    std::array<uint8_t, 8 + H5G_DENSE_FHEAP_ID_LEN> raw_corder{};
    ASSERT_GE(
        H5G_BT2_CORDER[0].encode(raw_corder.data(), &corder_rec, nullptr), 0);
    H5G_dense_bt2_corder_rec_t decoded_corder{};
    ASSERT_GE(
        H5G_BT2_CORDER[0].decode(raw_corder.data(), &decoded_corder, nullptr),
        0);
    EXPECT_EQ(decoded_corder.corder, corder_rec.corder);
    EXPECT_EQ(
        std::memcmp(decoded_corder.id, corder_rec.id, H5G_DENSE_FHEAP_ID_LEN),
        0);

    // Capture stream output; upstream H5Gbtree2.c writes trailing ID hex bytes
    // directly to stderr, so silence stderr temporarily while calling debug().
    char* buf = nullptr;
    size_t size = 0;
    FILE* memfp = open_memstream(&buf, &size);
    ASSERT_NE(memfp, nullptr);
    auto close_memfp = absl::MakeCleanup([&]() {
      if (memfp != nullptr) {
        std::fclose(memfp);
      }
      std::free(buf);
    });

    const int saved_stderr = dup(STDERR_FILENO);
    const int devnull = open("/dev/null", O_WRONLY);
    auto restore_stderr = absl::MakeCleanup([&]() {
      std::fflush(stderr);
      if (saved_stderr >= 0) {
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
      }
      if (devnull >= 0) {
        close(devnull);
      }
    });
    if (saved_stderr >= 0 && devnull >= 0) {
      std::fflush(stderr);
      dup2(devnull, STDERR_FILENO);
    }

    ASSERT_GE(H5G_BT2_NAME[0].debug(memfp, 2, 10, &name_rec, nullptr), 0);
    ASSERT_GE(H5G_BT2_CORDER[0].debug(memfp, 2, 10, &corder_rec, nullptr), 0);
    std::fclose(memfp);
    memfp = nullptr;

    std::string out(buf, size);
    EXPECT_THAT(out, HasSubstr("Record:"));
    EXPECT_THAT(out, HasSubstr("12345678"));
    EXPECT_THAT(out, HasSubstr("987654321"));
  }

  // 2. Error paths in H5G__dense_fh_name_cmp & H5G__dense_btree2_name_compare.
  ScopedFile file = CreateInMemoryFile("h5gbtree2_err.h5");
  ASSERT_TRUE(file.valid());
  ScopedPlist gcpl = MakeDenseLinkGroupCreatePlist();
  ASSERT_TRUE(gcpl.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/err_grp", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ASSERT_GE(
      H5Lcreate_soft("/t", grp.get(), "target", H5P_DEFAULT, H5P_DEFAULT), 0);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  H5G_loc_t gloc{};
  ASSERT_GE(H5G_loc(grp.get(), &gloc), 0);
  H5O_linfo_t linfo{};
  ASSERT_NE(H5O_msg_read(gloc.oloc, H5O_LINFO_ID, &linfo), nullptr);

  H5HF_t* fheap = H5HF_open(gloc.oloc->file, linfo.fheap_addr);
  ASSERT_NE(fheap, nullptr);
  auto close_fheap = absl::MakeCleanup([&]() { H5HF_close(fheap); });

  H5B2_t* bt2_name = H5B2_open(gloc.oloc->file, linfo.name_bt2_addr, nullptr);
  ASSERT_NE(bt2_name, nullptr);
  auto close_bt2 = absl::MakeCleanup([&]() { H5B2_close(bt2_name); });

  ScopedErrorSilence silence;

  // found_op returning < 0 hits HGOTO_ERROR in H5G__dense_fh_name_cmp and
  // propagates through H5HF_op in H5G__dense_btree2_name_compare. Upstream
  // jumps over H5O_msg_free(H5O_LINK_ID, lnk) on found_op failure, so release
  // the decoded link inside the test callback before returning -1.
  auto failing_found_op = [](const void* lnk, void* /*op_data*/) -> herr_t {
    H5O_msg_free(H5O_LINK_ID, const_cast<void*>(lnk));
    return -1;
  };
  H5G_bt2_ud_common_t udata{};
  udata.f = gloc.oloc->file;
  udata.fheap = fheap;
  udata.name = "target";
  udata.name_hash = H5_checksum_lookup3("target", std::strlen("target"), 0);
  udata.found_op = failing_found_op;
  udata.found_op_data = nullptr;

  bool found = false;
  EXPECT_LT(H5B2_find(bt2_name, &udata, &found, nullptr, nullptr), 0);

  // Corrupt link bytes in fractal heap fail H5O_msg_decode in
  // H5G__dense_fh_name_cmp.
  const std::array<uint8_t, 4> bad_bytes = {0xff, 0x00, 0xaa, 0x55};
  H5G_dense_bt2_name_rec_t rec{};
  ASSERT_GE(H5HF_insert(fheap, bad_bytes.size(), bad_bytes.data(), rec.id), 0);
  rec.hash = udata.name_hash;
  int cmp_result = 0;
  EXPECT_LT(H5G_BT2_NAME[0].compare(&udata, &rec, &cmp_result), 0);

  // Invalid fractal heap ID fails H5HF_op directly.
  std::memset(rec.id, 0x7f, sizeof(rec.id));
  EXPECT_LT(H5G_BT2_NAME[0].compare(&udata, &rec, &cmp_result), 0);
}

}  // namespace
}  // namespace hdf5
