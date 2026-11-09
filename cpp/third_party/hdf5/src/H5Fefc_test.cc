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
// Tests External File Cache (EFC) creation, LRU caching and eviction,
// selective cache release, cyclic dependency collection, and internal error
// handling (third_party/hdf5/src/H5Fefc.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Fefc.c

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug/stats fields in internal
// metadata cache structs embedded inside file structures.
#ifndef NDEBUG
#define H5FEFC_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5F_FRIEND
#define H5VL_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#pragma push_macro("try")
#undef try
#define try h5_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "third_party/hdf5/src/H5VLpkg.h"
}  // extern "C"
#ifdef H5FEFC_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FEFC_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  ScopedApiContext() { pushed_ = (H5CX_push(&node_) >= 0); }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

H5F_t* GetFileStruct(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

ScopedPlist MakeEfcFapl(unsigned efc_size) {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  if (fapl.valid() && H5Pset_elink_file_cache_size(fapl.get(), efc_size) < 0) {
    fapl.reset();
  }
  return fapl;
}

void CreateTargetFile(const std::string& path, const char* group_name = "g") {
  ScopedPlist fapl = MakeEfcFapl(4);
  ASSERT_TRUE(fapl.valid());
  ScopedFile file(H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gcreate2(file.get(), group_name, H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
}

void AddExternalLink(const std::string& src_path, const char* link_name,
                     const std::string& dst_path, const char* obj_name = "/") {
  ScopedFile file(H5Fopen(src_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
  ASSERT_TRUE(file.valid());
  ASSERT_GE(H5Lcreate_external(dst_path.c_str(), obj_name, file.get(), link_name,
                               H5P_DEFAULT, H5P_DEFAULT),
            0);
}

// Returns true iff `path` is not currently pinned read-only by any open handle
// or EFC entry (opening an already-RDONLY shared file with H5F_ACC_RDWR fails).
bool CanOpenReadWrite(const std::string& path) {
  ScopedErrorSilence silence;
  ScopedFile file(H5Fopen(path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
  return file.valid();
}

TEST(H5FefcTest, PublicLruCacheHitsEvictionsAndRelease) {
  TempDir dir("efc_lru");
  const std::string t0 = dir.child("t0.h5").native();
  const std::string t1 = dir.child("t1.h5").native();
  const std::string t2 = dir.child("t2.h5").native();
  const std::string root_path = dir.child("root.h5").native();

  CreateTargetFile(t0);
  CreateTargetFile(t1);
  CreateTargetFile(t2);

  ScopedPlist fapl = MakeEfcFapl(2);
  ASSERT_TRUE(fapl.valid());
  {
    ScopedFile root(
        H5Fcreate(root_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(root.valid());
    ASSERT_GE(H5Lcreate_external(t0.c_str(), "g", root.get(), "l0",
                                 H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_external(t1.c_str(), "g", root.get(), "l1",
                                 H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_external(t2.c_str(), "g", root.get(), "l2",
                                 H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // 1. Open root with EFC size = 2 and verify H5F__efc_max_nfiles via FAPL.
  ScopedFile root(
      H5Fopen(root_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(root.valid());
  {
    ScopedPlist got_fapl(H5Fget_access_plist(root.get()));
    ASSERT_TRUE(got_fapl.valid());
    unsigned efc_size = 0;
    ASSERT_GE(H5Pget_elink_file_cache_size(got_fapl.get(), &efc_size), 0);
    EXPECT_EQ(efc_size, 2u);
  }

  // 2. Populate cache: [l1, l0] -> hit tail (l0) -> [l0, l1] -> hit head (l0).
  for (const char* link : {"l0", "l1", "l0", "l0"}) {
    ScopedObject obj(H5Oopen(root.get(), link, H5P_DEFAULT));
    ASSERT_TRUE(obj.valid());
  }
  EXPECT_FALSE(CanOpenReadWrite(t0));
  EXPECT_FALSE(CanOpenReadWrite(t1));

  // 3. Opening l2 when cache [l0, l1] is full evicts LRU tail (l1) -> [l2, l0].
  {
    ScopedObject g2(H5Oopen(root.get(), "l2", H5P_DEFAULT));
    ASSERT_TRUE(g2.valid());
  }
  EXPECT_TRUE(CanOpenReadWrite(t1));
  EXPECT_FALSE(CanOpenReadWrite(t0));
  EXPECT_FALSE(CanOpenReadWrite(t2));

  // 4. Clearing the EFC releases cached files; an open external object on t2
  // keeps t2 open via weak close degree until g2_held closes, then t2 closes.
  {
    ScopedObject g2_held(H5Oopen(root.get(), "l2", H5P_DEFAULT));
    ASSERT_TRUE(g2_held.valid());
    ASSERT_GE(H5Fclear_elink_file_cache(root.get()), 0);
    EXPECT_TRUE(CanOpenReadWrite(t0));
    EXPECT_FALSE(CanOpenReadWrite(t2));
  }
  EXPECT_TRUE(CanOpenReadWrite(t2));

  // Re-opening after clear exercises non-NULL slist with nfiles == 0.
  {
    ScopedObject g0_again(H5Oopen(root.get(), "l0", H5P_DEFAULT));
    ASSERT_TRUE(g0_again.valid());
  }
  EXPECT_FALSE(CanOpenReadWrite(t0));
  root.reset();
  EXPECT_TRUE(CanOpenReadWrite(t0));

  // 5. Exercise size-3 middle hit (`[l2, l1, l0]` -> hit `l1`).
  ScopedPlist fapl3 = MakeEfcFapl(3);
  ASSERT_TRUE(fapl3.valid());
  ScopedFile root3(
      H5Fopen(root_path.c_str(), H5F_ACC_RDONLY, fapl3.get()));
  ASSERT_TRUE(root3.valid());
  for (const char* link : {"l0", "l1", "l2", "l1"}) {
    ScopedObject g(H5Oopen(root3.get(), link, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
  }
  EXPECT_FALSE(CanOpenReadWrite(t0));
  EXPECT_FALSE(CanOpenReadWrite(t1));
  EXPECT_FALSE(CanOpenReadWrite(t2));
  root3.reset();
  EXPECT_EQ(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_ALL), 0);
  EXPECT_TRUE(CanOpenReadWrite(t0));
  EXPECT_TRUE(CanOpenReadWrite(t1));
  EXPECT_TRUE(CanOpenReadWrite(t2));
}

TEST(H5FefcTest, SimpleAndThreeFileCyclesCloseOnFileClose) {
  TempDir dir("efc_cycles");
  const std::string a_path = dir.child("a.h5").native();
  const std::string b_path = dir.child("b.h5").native();
  const std::string c_path = dir.child("c.h5").native();

  CreateTargetFile(a_path);
  CreateTargetFile(b_path);
  CreateTargetFile(c_path);

  // Cycle A -> B -> C -> A, plus B -> A and A -> C so closing A exercises
  // reentry into H5F__efc_try_close(C) when C is already tagged CLOSE.
  AddExternalLink(a_path, "to_b", b_path);
  AddExternalLink(a_path, "to_c", c_path);
  AddExternalLink(b_path, "to_c", c_path);
  AddExternalLink(b_path, "to_a", a_path);
  AddExternalLink(c_path, "to_a", a_path);

  ScopedPlist fapl = MakeEfcFapl(4);
  ASSERT_TRUE(fapl.valid());
  ScopedFile a(H5Fopen(a_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(a.valid());

  for (const char* path :
       {"to_b", "to_b/to_c", "to_b/to_a", "to_c", "to_c/to_a"}) {
    ScopedObject obj(H5Oopen(a.get(), path, H5P_DEFAULT));
    ASSERT_TRUE(obj.valid()) << "Failed path: " << path;
  }

  // Closing A must break all cycles cleanly via Pass 1 + Pass 3 reentry.
  a.reset();
  EXPECT_EQ(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_ALL), 0);
  EXPECT_TRUE(CanOpenReadWrite(a_path));
  EXPECT_TRUE(CanOpenReadWrite(b_path));
  EXPECT_TRUE(CanOpenReadWrite(c_path));
}

TEST(H5FefcTest, Pass1PositiveTagAndMultipleHandleGuards) {
  TempDir dir("efc_tag_gt0");
  const std::string a_path = dir.child("a.h5").native();
  const std::string b_path = dir.child("b.h5").native();
  const std::string d_path = dir.child("d.h5").native();
  const std::string e_path = dir.child("e.h5").native();

  CreateTargetFile(a_path);
  CreateTargetFile(b_path);
  CreateTargetFile(d_path);
  CreateTargetFile(e_path);

  // D keeps references to A and E; A -> E -> B.
  // Closing A while D is open leaves A.efc->tag > 0 after Pass 1 (with E on
  // tmp_next list so reset iterates >1 nodes).
  AddExternalLink(a_path, "to_e", e_path);
  AddExternalLink(e_path, "to_b", b_path);
  AddExternalLink(d_path, "to_a", a_path);
  AddExternalLink(d_path, "to_e", e_path);

  ScopedPlist fapl = MakeEfcFapl(4);
  ASSERT_TRUE(fapl.valid());

  ScopedFile d(H5Fopen(d_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(d.valid());
  for (const char* link : {"to_a", "to_e"}) {
    ScopedObject obj(H5Oopen(d.get(), link, H5P_DEFAULT));
    ASSERT_TRUE(obj.valid());
  }

  {
    ScopedFile a1(H5Fopen(a_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(a1.valid());
    {
      ScopedObject obj(H5Oopen(a1.get(), "to_e/to_b", H5P_DEFAULT));
      ASSERT_TRUE(obj.valid());
    }
    ScopedFile a2(H5Fopen(a_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(a2.valid());
    a2.reset();  // nrefs > efc->nrefs + 1: returns immediately.
  }  // a1 closes while D still caches A and E -> A.efc->tag > 0 resets A and E.

  // D still keeps A, E, and B open in EFCs.
  EXPECT_EQ(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_FILE), 1);
  EXPECT_FALSE(CanOpenReadWrite(a_path));
  EXPECT_FALSE(CanOpenReadWrite(e_path));
  EXPECT_FALSE(CanOpenReadWrite(b_path));

  // Closing D releases everything cleanly.
  d.reset();
  EXPECT_EQ(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_ALL), 0);
  EXPECT_TRUE(CanOpenReadWrite(a_path));
  EXPECT_TRUE(CanOpenReadWrite(e_path));
  EXPECT_TRUE(CanOpenReadWrite(b_path));

  // Also verify closing a file whose EFC has nfiles == 0 while cached in D.
  CreateTargetFile(a_path);  // Empty EFC target (no external links).
  ScopedFile d_hold(H5Fopen(d_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(d_hold.valid());
  {
    ScopedFile empty_efc_a(H5Fopen(a_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(empty_efc_a.valid());
    ScopedObject via_d(H5Oopen(d_hold.get(), "to_a", H5P_DEFAULT));
    ASSERT_TRUE(via_d.valid());
    via_d.reset();
    // empty_efc_a closes with nrefs == 2 == efc->nrefs + 1 and nfiles == 0.
  }
  EXPECT_FALSE(CanOpenReadWrite(a_path));
  d_hold.reset();
  EXPECT_EQ(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_ALL), 0);
  EXPECT_TRUE(CanOpenReadWrite(a_path));
}

TEST(H5FefcTest, Pass2UncloseableGraphAndDontCloseReentry) {
  TempDir dir("efc_pass2");
  const std::string a_path = dir.child("a.h5").native();
  const std::string b_path = dir.child("b.h5").native();
  const std::string d_path = dir.child("d.h5").native();
  const std::string noefc_path = dir.child("noefc.h5").native();
  const std::string x_path = dir.child("x.h5").native();
  const std::string y_path = dir.child("y.h5").native();
  const std::string z_path = dir.child("z.h5").native();

  // Case A: X and Y are both pinned by open file D; X caches A, Z, and a
  // zero-EFC file `noefc`; Y also caches A; A -> B -> {X, Y}.
  // Closing A finds A.tag == 0 and X.tag == 1, Y.tag == 1 -> Pass 2 links
  // multiple nodes in uncloseable_head, marks A DONTCLOSE, skips `noefc`
  // (shared->efc == NULL), recurses into Z (DEFAULT), and cleans up.
  CreateTargetFile(a_path);
  CreateTargetFile(b_path);
  CreateTargetFile(d_path);
  CreateTargetFile(noefc_path);
  CreateTargetFile(x_path);
  CreateTargetFile(y_path);
  CreateTargetFile(z_path);

  AddExternalLink(a_path, "to_b", b_path);
  AddExternalLink(b_path, "to_x", x_path);
  AddExternalLink(b_path, "to_y", y_path);
  AddExternalLink(x_path, "to_a", a_path);
  AddExternalLink(x_path, "to_z", z_path);
  AddExternalLink(x_path, "to_noefc", noefc_path);
  AddExternalLink(y_path, "to_a", a_path);
  AddExternalLink(d_path, "to_x", x_path);
  AddExternalLink(d_path, "to_y", y_path);

  ScopedPlist fapl = MakeEfcFapl(4);
  ASSERT_TRUE(fapl.valid());

  ScopedFile d(H5Fopen(d_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(d.valid());
  {
    // Pre-open noefc with EFC size 0 so X's EFC holds a shared file whose
    // `shared->efc == nullptr`.
    ScopedPlist noefc_fapl = MakeEfcFapl(0);
    ASSERT_TRUE(noefc_fapl.valid());
    {
      ScopedFile noefc_ro(
          H5Fopen(noefc_path.c_str(), H5F_ACC_RDONLY, noefc_fapl.get()));
      ASSERT_TRUE(noefc_ro.valid());
      ScopedObject obj(H5Oopen(d.get(), "to_x/to_noefc", H5P_DEFAULT));
      ASSERT_TRUE(obj.valid());
    }

    ScopedFile a(H5Fopen(a_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(a.valid());
    for (const char* path : {"to_b", "to_b/to_x", "to_b/to_y"}) {
      ScopedObject obj(H5Oopen(a.get(), path, H5P_DEFAULT));
      ASSERT_TRUE(obj.valid()) << "Failed path: " << path;
    }
    for (const char* path :
         {"to_x", "to_y", "to_x/to_a", "to_y/to_a", "to_x/to_z"}) {
      ScopedObject obj(H5Oopen(d.get(), path, H5P_DEFAULT));
      ASSERT_TRUE(obj.valid()) << "Failed path: " << path;
    }

    // Closing A runs Pass 2: X and Y keep A alive in their EFCs.
    a.reset();
    EXPECT_EQ(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_FILE), 1);
    EXPECT_FALSE(CanOpenReadWrite(a_path));
    EXPECT_FALSE(CanOpenReadWrite(b_path));
    EXPECT_FALSE(CanOpenReadWrite(noefc_path));
  }
  // Closing D drops X & Y's external refs and allows the cycle to collapse.
  d.reset();
  EXPECT_EQ(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_ALL), 0);
  EXPECT_TRUE(CanOpenReadWrite(a_path));
  EXPECT_TRUE(CanOpenReadWrite(b_path));
  EXPECT_TRUE(CanOpenReadWrite(x_path));
  EXPECT_TRUE(CanOpenReadWrite(y_path));
  EXPECT_TRUE(CanOpenReadWrite(z_path));
  EXPECT_TRUE(CanOpenReadWrite(noefc_path));

  // Case B: DONTCLOSE reentry during release.
  // A2 caches B2 (B2 -> A2) and X2 (X2 -> Z2); D2 keeps X2 open.
  // Pass 1 sets A2.tag = 0, X2.tag = 1. Since X2 does not link back to A2,
  // Pass 2 leaves A2 tagged CLOSE and calls release_real(A2), which evicts X2
  // and reenters H5F__efc_try_close(X2) while X2 is tagged DONTCLOSE.
  const std::string a2_path = dir.child("a2.h5").native();
  const std::string b2_path = dir.child("b2.h5").native();
  const std::string d2_path = dir.child("d2.h5").native();
  const std::string x2_path = dir.child("x2.h5").native();
  const std::string z2_path = dir.child("z2.h5").native();

  CreateTargetFile(a2_path);
  CreateTargetFile(b2_path);
  CreateTargetFile(d2_path);
  CreateTargetFile(x2_path);
  CreateTargetFile(z2_path);

  AddExternalLink(a2_path, "to_b2", b2_path);
  AddExternalLink(a2_path, "to_x2", x2_path);
  AddExternalLink(b2_path, "to_a2", a2_path);
  AddExternalLink(d2_path, "to_x2", x2_path);
  AddExternalLink(x2_path, "to_z2", z2_path);

  ScopedFile d2(H5Fopen(d2_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(d2.valid());
  {
    ScopedFile a2(H5Fopen(a2_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(a2.valid());
    for (const char* path : {"to_b2", "to_b2/to_a2", "to_x2"}) {
      ScopedObject obj(H5Oopen(a2.get(), path, H5P_DEFAULT));
      ASSERT_TRUE(obj.valid()) << "Failed path: " << path;
    }
    for (const char* path : {"to_x2", "to_x2/to_z2"}) {
      ScopedObject obj(H5Oopen(d2.get(), path, H5P_DEFAULT));
      ASSERT_TRUE(obj.valid()) << "Failed path: " << path;
    }
    a2.reset();  // Closes A2 & B2; leaves D2 -> X2 -> Z2 open.
    EXPECT_EQ(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_FILE), 1);
    EXPECT_TRUE(CanOpenReadWrite(a2_path));
    EXPECT_TRUE(CanOpenReadWrite(b2_path));
    EXPECT_FALSE(CanOpenReadWrite(x2_path));
    EXPECT_FALSE(CanOpenReadWrite(z2_path));
  }
  d2.reset();
  EXPECT_EQ(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_ALL), 0);
  EXPECT_TRUE(CanOpenReadWrite(x2_path));
  EXPECT_TRUE(CanOpenReadWrite(z2_path));
}

TEST(H5FefcTest, DirectInternalApiErrorsNullEfcAndDestroyFailure) {
  ASSERT_GE(H5open(), 0);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  TempDir dir("efc_direct");
  const std::string t0 = dir.child("direct_t0.h5").native();
  const std::string t1 = dir.child("direct_t1.h5").native();
  const std::string missing = dir.child("missing.h5").native();
  const std::string parent_no_efc = dir.child("parent_no_efc.h5").native();

  CreateTargetFile(t0);
  CreateTargetFile(t1);
  CreateTargetFile(parent_no_efc);

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());

  // 1. Open and close with efc == nullptr (both existing and missing files).
  H5F_t* uncached = nullptr;
  ASSERT_GE(H5F__efc_open(false, nullptr, &uncached, t0.c_str(), H5F_ACC_RDONLY,
                          H5P_FILE_CREATE_DEFAULT, fapl.get()),
            0);
  ASSERT_NE(uncached, nullptr);

  H5F_t* missing_try = nullptr;
  EXPECT_GE(H5F__efc_open(true, nullptr, &missing_try, missing.c_str(),
                          H5F_ACC_RDONLY, H5P_FILE_CREATE_DEFAULT, fapl.get()),
            0);
  EXPECT_EQ(missing_try, nullptr);

  {
    ScopedFile parent(
        H5Fopen(parent_no_efc.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(parent.valid());
    H5F_t* parent_f = GetFileStruct(parent.get());
    ASSERT_NE(parent_f, nullptr);
    ASSERT_EQ(parent_f->shared->efc, nullptr);
    EXPECT_GE(H5F_efc_close(parent_f, uncached), 0);
  }

  // 2. Direct EFC creation, error checks, and destroy failure while open.
  H5F_efc_t* efc = H5F__efc_create(2);
  ASSERT_NE(efc, nullptr);
  EXPECT_EQ(H5F__efc_max_nfiles(efc), 2u);
  bool efc_destroyed = false;
  absl::Cleanup destroy_efc = [&] {
    if (!efc_destroyed) {
      H5F__efc_destroy(efc);
    }
  };

  // Non-file try-open miss on non-empty/new EFC frees temporary ent.
  EXPECT_GE(H5F__efc_open(true, efc, &missing_try, missing.c_str(),
                          H5F_ACC_RDONLY, H5P_FILE_CREATE_DEFAULT, fapl.get()),
            0);
  EXPECT_EQ(missing_try, nullptr);

  {
    ScopedErrorSilence silence;
    H5F_t* err_file = nullptr;
    EXPECT_LT(H5F__efc_open(false, efc, &err_file, t0.c_str(), H5F_ACC_RDONLY,
                            H5P_FILE_CREATE_DEFAULT, H5I_INVALID_HID),
              0);

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    EXPECT_LT(H5F__efc_open(false, efc, &err_file, t0.c_str(), H5F_ACC_RDONLY,
                            H5P_FILE_CREATE_DEFAULT, dcpl.get()),
              0);

    EXPECT_LT(H5F__efc_open(false, nullptr, &err_file, missing.c_str(),
                            H5F_ACC_RDONLY, H5P_FILE_CREATE_DEFAULT,
                            fapl.get()),
              0);
    EXPECT_LT(H5F__efc_open(false, efc, &err_file, missing.c_str(),
                            H5F_ACC_RDONLY, H5P_FILE_CREATE_DEFAULT,
                            fapl.get()),
              0);
  }

  // Open two entries inside efc using a parent with an active EFC of its own
  // (shared helper close requires decrementing ent->nopen).
  ScopedPlist efc_fapl = MakeEfcFapl(4);
  ASSERT_TRUE(efc_fapl.valid());
  ScopedFile parent(
      H5Fopen(parent_no_efc.c_str(), H5F_ACC_RDONLY, efc_fapl.get()));
  ASSERT_TRUE(parent.valid());
  H5F_t* parent_f = GetFileStruct(parent.get());
  ASSERT_NE(parent_f, nullptr);
  ASSERT_NE(parent_f->shared->efc, nullptr);

  H5F_t* cached_0 = nullptr;
  H5F_t* cached_1 = nullptr;
  H5F_efc_t* orig_efc = parent_f->shared->efc;
  parent_f->shared->efc = efc;
  absl::Cleanup restore_efc = [&] { parent_f->shared->efc = orig_efc; };

  ASSERT_GE(H5F__efc_open(false, efc, &cached_0, t0.c_str(), H5F_ACC_RDONLY,
                          H5P_FILE_CREATE_DEFAULT, fapl.get()),
            0);
  absl::Cleanup close_cached_0 = [&] {
    if (cached_0) {
      H5F_efc_close(parent_f, cached_0);
      cached_0 = nullptr;
    }
  };
  ASSERT_GE(H5F__efc_open(false, efc, &cached_1, t1.c_str(), H5F_ACC_RDONLY,
                          H5P_FILE_CREATE_DEFAULT, fapl.get()),
            0);
  absl::Cleanup close_cached_1 = [&] {
    if (cached_1) {
      H5F_efc_close(parent_f, cached_1);
      cached_1 = nullptr;
    }
  };

  // With all 2 cached entries open (none evictable), opening missing files hits
  // the uncached fallback branches (try=true returns NULL; try=false fails).
  EXPECT_GE(H5F__efc_open(true, efc, &missing_try, missing.c_str(),
                          H5F_ACC_RDONLY, H5P_FILE_CREATE_DEFAULT, fapl.get()),
            0);
  EXPECT_EQ(missing_try, nullptr);
  {
    ScopedErrorSilence silence;
    H5F_t* err_uncached = nullptr;
    EXPECT_LT(H5F__efc_open(false, efc, &err_uncached, missing.c_str(),
                            H5F_ACC_RDONLY, H5P_FILE_CREATE_DEFAULT,
                            fapl.get()),
              0);
    EXPECT_EQ(err_uncached, nullptr);
  }

  // H5F__efc_destroy fails while entries are still opened (nopen > 0).
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5F__efc_destroy(efc), 0);
  }

  ASSERT_GE(H5F_efc_close(parent_f, cached_0), 0);
  cached_0 = nullptr;
  ASSERT_GE(H5F_efc_close(parent_f, cached_1), 0);
  cached_1 = nullptr;
  parent_f->shared->efc = orig_efc;

  ASSERT_GE(H5F__efc_destroy(efc), 0);
  efc_destroyed = true;
}

TEST(H5FefcTest, DirectPinnedMiddleEvictionBypassAndSelectiveRelease) {
  ASSERT_GE(H5open(), 0);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  TempDir dir("efc_direct_pinned");
  const std::string t0 = dir.child("p0.h5").native();
  const std::string t1 = dir.child("p1.h5").native();
  const std::string t2 = dir.child("p2.h5").native();
  const std::string t3 = dir.child("p3.h5").native();
  const std::string t4 = dir.child("p4.h5").native();
  const std::string parent_path = dir.child("parent.h5").native();

  CreateTargetFile(t0);
  CreateTargetFile(t1);
  CreateTargetFile(t2);
  CreateTargetFile(t3);
  CreateTargetFile(t4);
  CreateTargetFile(parent_path);

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());

  ScopedPlist parent_fapl = MakeEfcFapl(4);
  ASSERT_TRUE(parent_fapl.valid());
  ScopedFile parent(
      H5Fopen(parent_path.c_str(), H5F_ACC_RDONLY, parent_fapl.get()));
  ASSERT_TRUE(parent.valid());
  H5F_t* parent_f = GetFileStruct(parent.get());
  ASSERT_NE(parent_f, nullptr);

  H5F_efc_t* efc = H5F__efc_create(3);
  ASSERT_NE(efc, nullptr);
  bool efc_destroyed = false;
  absl::Cleanup destroy_efc = [&] {
    if (!efc_destroyed) {
      H5F__efc_destroy(efc);
    }
  };

  H5F_efc_t* orig_efc = parent_f->shared->efc;
  parent_f->shared->efc = efc;
  absl::Cleanup restore_efc = [&] { parent_f->shared->efc = orig_efc; };

  H5F_t* f0 = nullptr;
  H5F_t* f1 = nullptr;
  H5F_t* f2 = nullptr;
  H5F_t* f3 = nullptr;
  H5F_t* f4_bypass = nullptr;
  absl::Cleanup close_open = [&] {
    if (f4_bypass) H5F_efc_close(parent_f, f4_bypass);
    if (f3) H5F_efc_close(parent_f, f3);
    if (f2) H5F_efc_close(parent_f, f2);
    if (f1) H5F_efc_close(parent_f, f1);
    if (f0) H5F_efc_close(parent_f, f0);
  };

  // 1. Populate size-3 EFC and keep tail t0 pinned (`ent->nopen == 1`) while t1
  // and t2 have `nopen == 0`: LRU = [t2 (head, 0), t1 (mid, 0), t0 (tail, 1)].
  ASSERT_GE(H5F__efc_open(false, efc, &f0, t0.c_str(), H5F_ACC_RDONLY,
                          H5P_FILE_CREATE_DEFAULT, fapl.get()),
            0);
  ASSERT_GE(H5F__efc_open(false, efc, &f1, t1.c_str(), H5F_ACC_RDONLY,
                          H5P_FILE_CREATE_DEFAULT, fapl.get()),
            0);
  ASSERT_GE(H5F_efc_close(parent_f, f1), 0);
  f1 = nullptr;
  ASSERT_GE(H5F__efc_open(false, efc, &f2, t2.c_str(), H5F_ACC_RDONLY,
                          H5P_FILE_CREATE_DEFAULT, fapl.get()),
            0);
  ASSERT_GE(H5F_efc_close(parent_f, f2), 0);
  f2 = nullptr;

  // 2. Opening t3 skips pinned tail t0 and evicts MIDDLE entry t1 (both
  // LRU_next and LRU_prev non-null in H5F__efc_remove_ent).
  ASSERT_GE(H5F__efc_open(false, efc, &f3, t3.c_str(), H5F_ACC_RDONLY,
                          H5P_FILE_CREATE_DEFAULT, fapl.get()),
            0);
  EXPECT_TRUE(CanOpenReadWrite(t1));
  EXPECT_FALSE(CanOpenReadWrite(t0));
  EXPECT_FALSE(CanOpenReadWrite(t2));
  EXPECT_FALSE(CanOpenReadWrite(t3));

  // 3. Re-pin t2 (`nopen == 1`) so all 3 slots [t2(1), t3(1), t0(1)] are
  // pinned; opening t4 bypasses EFC, and H5F_efc_close closes t4 immediately.
  ASSERT_GE(H5F__efc_open(false, efc, &f2, t2.c_str(), H5F_ACC_RDONLY,
                          H5P_FILE_CREATE_DEFAULT, fapl.get()),
            0);
  ASSERT_GE(H5F__efc_open(false, efc, &f4_bypass, t4.c_str(), H5F_ACC_RDONLY,
                          H5P_FILE_CREATE_DEFAULT, fapl.get()),
            0);
  EXPECT_FALSE(CanOpenReadWrite(t4));
  ASSERT_GE(H5F_efc_close(parent_f, f4_bypass), 0);
  f4_bypass = nullptr;
  EXPECT_TRUE(CanOpenReadWrite(t4));

  // 4. Close t2 (`nopen -> 0`) while keeping t3 and t0 pinned (`nopen == 1`);
  // H5F__efc_release evicts t2 and skips t3 and t0.
  ASSERT_GE(H5F_efc_close(parent_f, f2), 0);
  f2 = nullptr;
  ASSERT_GE(H5F__efc_release(efc), 0);
  EXPECT_TRUE(CanOpenReadWrite(t2));
  EXPECT_FALSE(CanOpenReadWrite(t3));
  EXPECT_FALSE(CanOpenReadWrite(t0));

  // 5. Unpin t3 and t0 and destroy the EFC cleanly.
  ASSERT_GE(H5F_efc_close(parent_f, f3), 0);
  f3 = nullptr;
  ASSERT_GE(H5F_efc_close(parent_f, f0), 0);
  f0 = nullptr;
  parent_f->shared->efc = orig_efc;

  ASSERT_GE(H5F__efc_destroy(efc), 0);
  efc_destroyed = true;
  EXPECT_TRUE(CanOpenReadWrite(t0));
  EXPECT_TRUE(CanOpenReadWrite(t3));
}

herr_t DummyGetWrapCtx(const void* /*obj*/, void** wrap_ctx) {
  static int kWrapCtxSentinel = 1;
  *wrap_ctx = &kWrapCtxSentinel;
  return 0;
}

void* FailingWrapObject(void* /*obj*/, H5I_type_t /*obj_type*/,
                        void* /*wrap_ctx*/) {
  return nullptr;
}

herr_t DummyFreeWrapCtx(void* /*wrap_ctx*/) { return 0; }

TEST(H5FefcTest, PostOpenFailureClosesOpenedFileCleanly) {
  ASSERT_GE(H5open(), 0);
  TempFile target("post_open_fail.h5");
  CreateTargetFile(target.path().native());

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());

  H5VL_class_t wrap_fail_cls{};
  wrap_fail_cls.wrap_cls.get_wrap_ctx = DummyGetWrapCtx;
  wrap_fail_cls.wrap_cls.wrap_object = FailingWrapObject;
  wrap_fail_cls.wrap_cls.free_wrap_ctx = DummyFreeWrapCtx;

  H5VL_connector_t dummy_conn{};
  dummy_conn.cls = &wrap_fail_cls;
  dummy_conn.nrefs = 2;

  H5VL_object_t dummy_vol_obj{};
  dummy_vol_obj.data = &wrap_fail_cls;
  dummy_vol_obj.connector = &dummy_conn;
  dummy_vol_obj.rc = 1;

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ASSERT_GE(H5VL_set_vol_wrapper(&dummy_vol_obj), 0);
    absl::Cleanup reset_wrap = [] { H5VL_reset_vol_wrapper(); };

    ScopedErrorSilence silence;
    H5F_t* out_file = nullptr;
    EXPECT_LT(
        H5F__efc_open(false, nullptr, &out_file, target.c_str(),
                      H5F_ACC_RDONLY, H5P_FILE_CREATE_DEFAULT, fapl.get()),
        0);
    EXPECT_EQ(out_file, nullptr);
  }

  EXPECT_EQ(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_ALL), 0);
  EXPECT_TRUE(CanOpenReadWrite(target.path().native()));
}

}  // namespace
}  // namespace hdf5
