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
// Tests v2 B-tree dense attribute indexing callbacks (name hash index,
// creation order index, encode/decode serialization, shared SOHM attribute
// reconstitution, lookup3 hash collisions, and debug/error paths) for
// third_party/hdf5/src/H5Abtree2.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Abtree2.c

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from cache
// entries in internal headers. Match those struct layouts here.
#ifndef NDEBUG
#define H5ABTREE2_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

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

// H5B2private.h uses `try` as a C parameter name, which collides with both the
// C++ keyword and Google3's `-fno-exceptions` `try` macro.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_param_try
extern "C" {
#define H5A_FRIEND
#include "third_party/hdf5/src/H5Apkg.h"
#include "third_party/hdf5/src/H5B2private.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5HFprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5ABTREE2_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ABTREE2_TEST_RESTORE_UNDEF_NDEBUG
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

ScopedPlist MakeDenseGroupCreatePlist() {
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  if (!gcpl.valid()) {
    return ScopedPlist();
  }
  if (H5Pset_attr_phase_change(gcpl.get(), 0, 0) < 0 ||
      H5Pset_attr_creation_order(
          gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
    gcpl.reset();
  }
  return gcpl;
}

TEST(H5Abtree2Test, DenseNameAndCorderOperationsAndReopen) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl = MakeDenseGroupCreatePlist();
  ASSERT_TRUE(gcpl.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/dense_grp", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  // Create enough dense attributes to span multiple 512-byte v2 B-tree nodes
  // in both H5A_BT2_NAME and H5A_BT2_CORDER, exercising store and compare.
  constexpr int kNumAttrs = 70;
  for (int i = 0; i < kNumAttrs; ++i) {
    std::string name = absl::StrFormat("attr_%03d", i);
    ScopedAttr attr(H5Acreate2(grp.get(), name.c_str(), H5T_NATIVE_INT,
                               scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid()) << "Failed creating " << name;
    ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT, &i), 0);
  }

  // Probe missing attributes (with varied hashes) and update existing ones.
  EXPECT_EQ(H5Aexists(grp.get(), "missing_low_hash"), 0);
  EXPECT_EQ(H5Aexists(grp.get(), "zzzz_missing_high_hash"), 0);
  {
    ScopedAttr attr(H5Aopen(grp.get(), "attr_035", H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    int updated = 3535;
    ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT, &updated), 0);
  }

  // Rename an attribute and delete several across both indices before flush.
  ASSERT_GE(H5Arename(grp.get(), "attr_000", "attr_000_renamed"), 0);
  ASSERT_GE(H5Adelete(grp.get(), "attr_010"), 0);
  ASSERT_GE(H5Adelete_by_idx(grp.get(), ".", H5_INDEX_CRT_ORDER, H5_ITER_DEC, 0,
                             H5P_DEFAULT),
            0);

  grp.reset();

  // Flush and extract the serialized in-memory image, forcing encode callbacks
  // on both H5A_BT2_NAME and H5A_BT2_CORDER nodes, then reopen to force decode.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
            image_size);
  file.reset();

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("reopen_dense.h5", H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedGroup reopened_grp(H5Gopen2(reopened.get(), "/dense_grp", H5P_DEFAULT));
  ASSERT_TRUE(reopened_grp.valid());

  // Verify lookups after reopen using both name and creation order indexes.
  EXPECT_EQ(H5Aexists(reopened_grp.get(), "attr_000_renamed"), 1);
  EXPECT_EQ(H5Aexists(reopened_grp.get(), "attr_010"), 0);

  ScopedAttr a35(H5Aopen(reopened_grp.get(), "attr_035", H5P_DEFAULT));
  ASSERT_TRUE(a35.valid());
  int val = -1;
  ASSERT_GE(H5Aread(a35.get(), H5T_NATIVE_INT, &val), 0);
  EXPECT_EQ(val, 3535);

  H5A_info_t info{};
  ASSERT_GE(H5Aget_info_by_idx(reopened_grp.get(), ".", H5_INDEX_CRT_ORDER,
                               H5_ITER_INC, 0, &info, H5P_DEFAULT),
            0);
  EXPECT_TRUE(info.corder_valid);

  char name_buf[64] = {};
  ssize_t nlen = H5Aget_name_by_idx(reopened_grp.get(), ".", H5_INDEX_NAME,
                                    H5_ITER_DEC, 1, name_buf, sizeof(name_buf),
                                    H5P_DEFAULT);
  EXPECT_GT(nlen, 0);

  std::vector<std::string> iter_names;
  auto collect_cb = [](hid_t /*loc_id*/, const char* attr_name,
                       const H5A_info_t* /*ainfo*/, void* op_data) -> herr_t {
    static_cast<std::vector<std::string>*>(op_data)->emplace_back(attr_name);
    return 0;
  };
  hsize_t idx = 0;
  ASSERT_GE(H5Aiterate2(reopened_grp.get(), H5_INDEX_CRT_ORDER, H5_ITER_INC,
                        &idx, collect_cb, &iter_names),
            0);
  EXPECT_EQ(iter_names.size(), kNumAttrs - 2);

  // H5_ITER_NATIVE on H5_INDEX_CRT_ORDER walks bt2_corder directly, exercising
  // H5A__dense_btree2_corder_decode after reopen.
  std::vector<std::string> native_names;
  hsize_t native_idx = 0;
  ASSERT_GE(H5Aiterate2(reopened_grp.get(), H5_INDEX_CRT_ORDER, H5_ITER_NATIVE,
                        &native_idx, collect_cb, &native_names),
            0);
  EXPECT_EQ(native_names.size(), kNumAttrs - 2);
}

TEST(H5Abtree2Test, Lookup3HashCollisionExercisesFhNameCmpMismatch) {
  // "k_2499" and "k_87023" collide under H5_checksum_lookup3(..., 0).
  constexpr const char* kSmallName = "k_2499";
  constexpr const char* kLargeName = "k_87023";
  ASSERT_LT(std::strcmp(kSmallName, kLargeName), 0);
  ASSERT_EQ(H5_checksum_lookup3(kSmallName, std::strlen(kSmallName), 0),
            H5_checksum_lookup3(kLargeName, std::strlen(kLargeName), 0));

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedPlist gcpl = MakeDenseGroupCreatePlist();
  ASSERT_TRUE(gcpl.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/collide", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  // Start with kLargeName stored; probing kSmallName hits udata->cmp < 0.
  {
    ScopedAttr attr(H5Acreate2(grp.get(), kLargeName, H5T_NATIVE_INT,
                               scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    int v = 2;
    ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT, &v), 0);
  }
  EXPECT_EQ(H5Aexists(grp.get(), kSmallName), 0);

  // Insert colliding kSmallName as well; both coexist with identical hashes.
  {
    ScopedAttr attr(H5Acreate2(grp.get(), kSmallName, H5T_NATIVE_INT,
                               scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    int v = 1;
    ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT, &v), 0);
  }

  EXPECT_EQ(H5Aexists(grp.get(), kSmallName), 1);
  EXPECT_EQ(H5Aexists(grp.get(), kLargeName), 1);

  ScopedAttr a1(H5Aopen(grp.get(), kSmallName, H5P_DEFAULT));
  ASSERT_TRUE(a1.valid());
  int v1 = 0;
  ASSERT_GE(H5Aread(a1.get(), H5T_NATIVE_INT, &v1), 0);
  EXPECT_EQ(v1, 1);

  ScopedAttr a2(H5Aopen(grp.get(), kLargeName, H5P_DEFAULT));
  ASSERT_TRUE(a2.valid());
  int v2 = 0;
  ASSERT_GE(H5Aread(a2.get(), H5T_NATIVE_INT, &v2), 0);
  EXPECT_EQ(v2, 2);

  // Remove kLargeName and probe kLargeName vs kSmallName to exercise cmp > 0.
  a2.reset();
  ASSERT_GE(H5Adelete(grp.get(), kLargeName), 0);
  EXPECT_EQ(H5Aexists(grp.get(), kLargeName), 0);
  EXPECT_EQ(H5Aexists(grp.get(), kSmallName), 1);
}

TEST(H5Abtree2Test, SharedDenseAttributeReconstitution) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 1),
            0);

  ScopedFile file = CreateInMemoryFile("shared_dense.h5", H5F_ACC_TRUNC, fcpl);
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl = MakeDenseGroupCreatePlist();
  ASSERT_TRUE(gcpl.valid());
  ScopedGroup g1(
      H5Gcreate2(file.get(), "/g1", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(g1.valid());
  ScopedGroup g2(
      H5Gcreate2(file.get(), "/g2", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(g2.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  // Creating dense attributes in a SOHM-enabled file marks records with
  // H5O_MSG_FLAG_SHARED and exercises shared_fheap + H5SM_reconstitute.
  for (int i = 0; i < 6; ++i) {
    std::string name = absl::StrFormat("shared_attr_%02d", i);
    for (hid_t g : {g1.get(), g2.get()}) {
      ScopedAttr attr(H5Acreate2(g, name.c_str(), H5T_NATIVE_INT, scalar.get(),
                                 H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(attr.valid());
      int v = 100 + i;
      ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT, &v), 0);
    }
  }

  for (int i = 0; i < 6; ++i) {
    std::string name = absl::StrFormat("shared_attr_%02d", i);
    ScopedAttr attr(H5Aopen(g1.get(), name.c_str(), H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    int v = 0;
    ASSERT_GE(H5Aread(attr.get(), H5T_NATIVE_INT, &v), 0);
    EXPECT_EQ(v, 100 + i);
  }
}

TEST(H5Abtree2Test, DirectDebugAndErrorCallbacks) {
  // Verify the debug record printers for both v2 B-tree classes.
  {
    H5A_dense_bt2_name_rec_t name_rec{};
    name_rec.id.val = 0x0102030405060708ULL;
    name_rec.flags = 0x05;
    name_rec.corder = 17;
    name_rec.hash = 0xabcdef01U;

    H5A_dense_bt2_corder_rec_t corder_rec{};
    corder_rec.id.val = 0x1112131415161718ULL;
    corder_rec.flags = 0x02;
    corder_rec.corder = 42;

    char* buf = nullptr;
    size_t size = 0;
    FILE* memfp = open_memstream(&buf, &size);
    ASSERT_NE(memfp, nullptr);
    auto close_stream = absl::MakeCleanup([&]() {
      if (memfp != nullptr) {
        std::fclose(memfp);
      }
      std::free(buf);
    });

    ASSERT_GE(H5A_BT2_NAME[0].debug(memfp, 2, 10, &name_rec, nullptr), 0);
    ASSERT_GE(H5A_BT2_CORDER[0].debug(memfp, 2, 10, &corder_rec, nullptr), 0);
    std::fclose(memfp);
    memfp = nullptr;

    std::string out(buf, size);
    EXPECT_THAT(out, HasSubstr("Record:"));
    EXPECT_THAT(out, HasSubstr("abcdef01"));
    EXPECT_THAT(out, HasSubstr(", 17,"));
    EXPECT_THAT(out, HasSubstr(", 42}"));
  }

  // Exercise H5A__dense_fh_name_cmp error paths (found_op failure, invalid
  // heap ID -> H5HF_op failure, and corrupt bytes -> H5O_msg_decode failure).
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedPlist gcpl = MakeDenseGroupCreatePlist();
  ASSERT_TRUE(gcpl.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/err_grp", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedAttr attr(H5Acreate2(grp.get(), "target", H5T_NATIVE_INT, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  int v = 99;
  ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT, &v), 0);
  attr.reset();

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  H5G_loc_t gloc{};
  ASSERT_GE(H5G_loc(grp.get(), &gloc), 0);
  H5O_ainfo_t ainfo{};
  ASSERT_NE(H5O_msg_read(gloc.oloc, H5O_AINFO_ID, &ainfo), nullptr);

  H5HF_t* fheap = H5HF_open(gloc.oloc->file, ainfo.fheap_addr);
  ASSERT_NE(fheap, nullptr);
  auto close_fheap = absl::MakeCleanup([&]() { H5HF_close(fheap); });

  H5B2_t* bt2_name = H5B2_open(gloc.oloc->file, ainfo.name_bt2_addr, nullptr);
  ASSERT_NE(bt2_name, nullptr);
  auto close_bt2 = absl::MakeCleanup([&]() { H5B2_close(bt2_name); });

  ScopedErrorSilence silence;

  // 1. found_op returning < 0 hits HGOTO_ERROR in H5A__dense_fh_name_cmp and
  //    propagates through H5HF_op in H5A__dense_btree2_name_compare.
  auto failing_found_op = [](const H5A_t* /*attr*/, bool* /*took_ownership*/,
                             void* /*op_data*/) -> herr_t { return -1; };
  H5A_bt2_ud_common_t udata{};
  udata.f = gloc.oloc->file;
  udata.fheap = fheap;
  udata.shared_fheap = nullptr;
  udata.name = "target";
  udata.name_hash = H5_checksum_lookup3("target", std::strlen("target"), 0);
  udata.found_op = failing_found_op;
  udata.found_op_data = nullptr;

  bool found = false;
  EXPECT_LT(H5B2_find(bt2_name, &udata, &found, nullptr, nullptr), 0);

  // 2. Corrupt heap object fails H5O_msg_decode in H5A__dense_fh_name_cmp.
  const uint8_t bad_bytes[4] = {0xff, 0x00, 0xaa, 0x55};
  H5O_fheap_id_t bad_attr_id{};
  ASSERT_GE(H5HF_insert(fheap, sizeof(bad_bytes), bad_bytes, &bad_attr_id), 0);

  H5A_dense_bt2_name_rec_t rec{};
  rec.id = bad_attr_id;
  rec.hash = udata.name_hash;
  int cmp_result = 0;
  EXPECT_LT(H5A_BT2_NAME[0].compare(&udata, &rec, &cmp_result), 0);

  // 3. Invalid fractal heap ID fails H5HF_op directly.
  std::memset(&rec.id, 0x7f, sizeof(rec.id));
  EXPECT_LT(H5A_BT2_NAME[0].compare(&udata, &rec, &cmp_result), 0);
}

}  // namespace
}  // namespace hdf5
