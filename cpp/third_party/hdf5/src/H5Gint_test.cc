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
// Tests internal H5G group lifecycle, open/close refcounting, iteration,
// recursive visit, creation property list reconstruction, and info retrieval
// (third_party/hdf5/src/H5Gint.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Gint.c

#ifndef NDEBUG
#define H5GINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5F_FRIEND
#define H5G_FRIEND
#define H5O_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FDdevelop.h"
#include "third_party/hdf5/src/H5FOprivate.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try h5g_test_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Gpkg.h"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Ldevelop.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5GINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5GINT_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

H5F_t* UnwrapFile(const ScopedFile& file) {
  return static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
}

H5G_t* UnwrapGroup(const ScopedGroup& grp) {
  return static_cast<H5G_t*>(H5VL_object_verify(grp.get(), H5I_GROUP));
}

TEST(H5GintTest, InitAccessorsAndMountFlags) {
  ASSERT_GE(H5G_init(), 0);
  EXPECT_EQ(H5G_oloc(nullptr), nullptr);
  EXPECT_EQ(H5G_nameof(nullptr), nullptr);

  ScopedFile file = CreateInMemoryFile("gint_acc.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gcreate2(file.get(), "g", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  H5G_t* g = UnwrapGroup(grp);
  ASSERT_NE(g, nullptr);
  EXPECT_NE(H5G_oloc(g), nullptr);
  EXPECT_NE(H5G_nameof(g), nullptr);
  EXPECT_EQ(H5G_fileof(g), UnwrapFile(file));
  EXPECT_EQ(H5G_get_shared_count(g), 1);

  H5G_obj_create_t gcrt{};
  gcrt.gcpl_id = H5P_GROUP_CREATE;
  EXPECT_EQ(H5G_get_gcpl_id(&gcrt), H5P_GROUP_CREATE);

  EXPECT_FALSE(H5G_mounted(g));
  ASSERT_GE(H5G_mount(g), 0);
  EXPECT_TRUE(H5G_mounted(g));
  ASSERT_GE(H5G_unmount(g), 0);
  EXPECT_FALSE(H5G_mounted(g));
}

TEST(H5GintTest, CreateAndOpenErrorPathsAndReadOnlyFailure) {
  TempFile tmp("gint_ro.h5");
  const std::string& path = tmp.string();
  {
    ScopedFile rw(H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                            H5P_DEFAULT));
    ASSERT_TRUE(rw.valid());
    ScopedSpace space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(rw.get(), "dset", H5T_NATIVE_INT, space.get(),
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ScopedGroup grp(H5Gcreate2(rw.get(), "grp", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
  }

  ScopedFile ro(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(ro.valid());
  H5CX_node_t api_ctx{};
  ASSERT_GE(H5CX_push(&api_ctx), 0);
  absl::Cleanup pop_ctx = [] { H5CX_pop(false); };
  H5F_t* f_ro = UnwrapFile(ro);
  ASSERT_NE(f_ro, nullptr);

  H5G_loc_t file_loc{};
  ASSERT_GE(H5G_loc(ro.get(), &file_loc), 0);
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());

  ScopedErrorSilence silence;
  // 1. H5G__create fails on read-only file inside H5G__obj_create.
  H5G_obj_create_t gcrt{};
  gcrt.gcpl_id = gcpl.get();
  EXPECT_EQ(H5G__create(f_ro, &gcrt), nullptr);

  // 2. H5G__create_named fails on duplicate or read-only.
  EXPECT_EQ(H5G__create_named(&file_loc, "grp", lcpl.get(), gcpl.get()),
            nullptr);

  // 3. H5G__open_name fails on missing name and on dataset (not a group).
  EXPECT_EQ(H5G__open_name(&file_loc, "missing"), nullptr);
  EXPECT_EQ(H5G__open_name(&file_loc, "dset"), nullptr);

  // 4. H5G_open directly on dataset location fails inside H5G__open_oid.
  H5G_loc_t dset_loc{};
  H5G_name_t dset_path{};
  H5O_loc_t dset_oloc{};
  dset_loc.oloc = &dset_oloc;
  dset_loc.path = &dset_path;
  H5G_loc_reset(&dset_loc);
  ASSERT_GE(H5G_loc_find(&file_loc, "dset", &dset_loc), 0);
  // H5G_open takes ownership via shallow copy and frees on error.
  EXPECT_EQ(H5G_open(&dset_loc), nullptr);

  // 5. H5G_open on invalid object header address fails in H5O_open.
  H5G_loc_t bad_loc{};
  H5G_name_t bad_path{};
  H5O_loc_t bad_oloc{};
  bad_loc.oloc = &bad_oloc;
  bad_loc.path = &bad_path;
  H5G_loc_reset(&bad_loc);
  bad_oloc.file = f_ro;
  bad_oloc.addr = HADDR_UNDEF;
  EXPECT_EQ(H5G_open(&bad_loc), nullptr);

  // 6. H5G__get_info_by_name / idx errors on missing and dataset targets.
  H5G_info_t ginfo{};
  EXPECT_LT(H5G__get_info_by_name(&file_loc, "missing", &ginfo), 0);
  EXPECT_LT(H5G__get_info_by_name(&file_loc, "dset", &ginfo), 0);
  EXPECT_LT(H5G__get_info_by_idx(&file_loc, ".", H5_INDEX_NAME, H5_ITER_INC, 99,
                                 &ginfo),
            0);
  EXPECT_LT(H5G__get_info_by_idx(&file_loc, ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                                 &ginfo),
            0);
}

TEST(H5GintTest, ReopenTopCountCorkEvictOnCloseAndMountClose) {
  // 1. Test top_count == 0 and > 0 branches in H5G_open / H5G_close via
  //    H5Freopen, plus H5Odisable_mdc_flushes (cork) and evict_on_close.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_evict_on_close(fapl.get(), true), 0);

  ScopedFile f1(H5Fcreate("gint_reopen.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                          fapl.get()));
  ASSERT_TRUE(f1.valid());

  {
    ScopedGroup g1(H5Gcreate2(f1.get(), "g", H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT));
    ASSERT_TRUE(g1.valid());
    EXPECT_EQ(H5G_get_shared_count(UnwrapGroup(g1)), 1);

    // Cork the group so H5G_close triggers H5AC__UNCORK.
    ASSERT_GE(H5Odisable_mdc_flushes(g1.get()), 0);

    ScopedFile f2(H5Freopen(f1.get()));
    ASSERT_TRUE(f2.valid());

    // Opening via f2 hits H5FO_opened != NULL with H5FO_top_count == 0.
    ScopedGroup g2a(H5Gopen2(f2.get(), "g", H5P_DEFAULT));
    ASSERT_TRUE(g2a.valid());
    EXPECT_EQ(H5G_get_shared_count(UnwrapGroup(g1)), 2);

    // Second open via f2 makes top_count(f2) == 2.
    ScopedGroup g2b(H5Gopen2(f2.get(), "g", H5P_DEFAULT));
    ASSERT_TRUE(g2b.valid());
    EXPECT_EQ(H5G_get_shared_count(UnwrapGroup(g1)), 3);

    // Closing g2a leaves top_count(f2) > 0 -> hits H5O_loc_free branch.
    g2a.reset();
    // Closing g2b drops top_count(f2) == 0 while shared->fo_count > 0.
    g2b.reset();
  }
  // Closing g1 drops fo_count == 0 -> hits uncork and evict_on_close flush/evict.

  // 2. Test mounted && fo_count == 1 deferred H5F_try_close in H5G_close.
  ScopedFile parent = CreateInMemoryFile("mount_parent.h5");
  ASSERT_TRUE(parent.valid());
  ScopedGroup mp(H5Gcreate2(parent.get(), "mp", H5P_DEFAULT, H5P_DEFAULT,
                            H5P_DEFAULT));
  ASSERT_TRUE(mp.valid());
  ScopedFile child = CreateInMemoryFile("mount_child.h5");
  ASSERT_TRUE(child.valid());
  ASSERT_GE(H5Fmount(parent.get(), "mp", child.get(), H5P_DEFAULT), 0);

  // Close child and parent IDs while mp remains open as the last ref.
  child.reset();
  parent.reset();
  // Closing mp triggers H5G_close with mounted == true and fo_count == 1.
  mp.reset();
}

TEST(H5GintTest, IterateAndVisitTraversalAndIndexFallbacks) {
  // Build a file supporting tracked creation order at root, containing:
  // - untracked new-format subgroup ("new_untracked")
  // - symbol-table old-format subgroup ("old_stab")
  // - cycle back to root and soft link
  // - deeply nested group with long names (> 64 chars) to force realloc
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(
      H5Pset_link_creation_order(fcpl.get(),
                                 H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
      0);
  ScopedFile file =
      CreateInMemoryFile("gint_visit.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl_untracked(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl_untracked.valid());
  ASSERT_GE(H5Pset_link_phase_change(gcpl_untracked.get(), 10, 5), 0);

  ScopedGroup g_untracked(H5Gcreate2(file.get(), "new_untracked", H5P_DEFAULT,
                                     gcpl_untracked.get(), H5P_DEFAULT));
  ASSERT_TRUE(g_untracked.valid());

  ScopedGroup g_stab(H5Gcreate2(file.get(), "old_stab", H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g_stab.valid());

  const std::string long_name(96, 'L');
  ScopedGroup g_long(H5Gcreate2(g_stab.get(), long_name.c_str(), H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g_long.valid());

  // Cycle link back to root and a soft link.
  ASSERT_GE(H5Lcreate_hard(file.get(), "/", g_long.get(), "cycle_root",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_soft("/new_untracked", g_untracked.get(), "soft_link",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  g_long.reset();
  g_stab.reset();
  g_untracked.reset();

  // 1. Visit with H5_INDEX_CRT_ORDER from root (tracked) -> recurses into
  //    new_untracked (linfo with !track_corder) and old_stab (no linfo).
  std::vector<std::string> crt_paths;
  auto collect_visit = [](hid_t /*g*/, const char* path,
                          const H5L_info2_t* /*info*/, void* data) -> herr_t {
    static_cast<std::vector<std::string>*>(data)->emplace_back(path);
    return H5_ITER_CONT;
  };
  ASSERT_GE(H5Lvisit2(file.get(), H5_INDEX_CRT_ORDER, H5_ITER_INC,
                      collect_visit, &crt_paths),
            0);
  EXPECT_THAT(crt_paths, testing::Contains("new_untracked/soft_link"));
  EXPECT_THAT(crt_paths, testing::Contains("old_stab/" + long_name +
                                           "/cycle_root"));

  // 2. Public H5Lvisit_by_name2 starting at new_untracked (top-level
  //    !track_corder fallback) and old_stab (top-level !linfo_exists fallback).
  crt_paths.clear();
  ASSERT_GE(H5Lvisit_by_name2(file.get(), "new_untracked", H5_INDEX_CRT_ORDER,
                              H5_ITER_INC, collect_visit, &crt_paths,
                              H5P_DEFAULT),
            0);
  EXPECT_THAT(crt_paths, ElementsAre("soft_link"));

  crt_paths.clear();
  ASSERT_GE(H5Lvisit_by_name2(file.get(), "old_stab", H5_INDEX_CRT_ORDER,
                              H5_ITER_INC, collect_visit, &crt_paths,
                              H5P_DEFAULT),
            0);
  EXPECT_THAT(crt_paths, testing::Contains(long_name));

  // 3. Visit with H5_INDEX_NAME and early stop (> 0).
  auto stop_cb = [](hid_t /*g*/, const char* /*path*/,
                    const H5L_info2_t* /*info*/, void* /*data*/) -> herr_t {
    return 42;
  };
  EXPECT_EQ(
      H5Lvisit2(file.get(), H5_INDEX_NAME, H5_ITER_INC, stop_cb, nullptr), 42);

  // 4. H5G_iterate via H5Literate2 (NEW) and H5Giterate (OLD).
  auto iter_new_cb = [](hid_t /*g*/, const char* name,
                        const H5L_info2_t* /*info*/, void* d) -> herr_t {
    static_cast<std::vector<std::string>*>(d)->emplace_back(name);
    return H5_ITER_CONT;
  };
  std::vector<std::string> iter_names;
  hsize_t last_lnk = 0;
  ASSERT_GE(H5Literate2(file.get(), H5_INDEX_NAME, H5_ITER_INC, &last_lnk,
                        iter_new_cb, &iter_names),
            0);
  EXPECT_EQ(iter_names.size(), 2u);

  auto iter_old_cb = [](hid_t /*g*/, const char* name, void* d) -> herr_t {
    static_cast<std::vector<std::string>*>(d)->emplace_back(name);
    return H5_ITER_CONT;
  };
  iter_names.clear();
  int old_idx = 0;
  ASSERT_GE(H5Giterate(file.get(), ".", &old_idx, iter_old_cb, &iter_names), 0);
  EXPECT_EQ(iter_names.size(), 2u);

  // 5. Error paths in H5G_visit and H5G_iterate.
  H5CX_node_t api_ctx{};
  ASSERT_GE(H5CX_push(&api_ctx), 0);
  absl::Cleanup pop_ctx = [] { H5CX_pop(false); };
  H5G_loc_t root_loc{};
  ASSERT_GE(H5G_loc(file.get(), &root_loc), 0);
  H5G_link_iterate_t op_new{};
  op_new.op_type = H5G_LINK_OP_NEW;
  op_new.op_func.op_new = iter_new_cb;

  ScopedErrorSilence silence;
  EXPECT_LT(H5G_visit(nullptr, ".", H5_INDEX_NAME, H5_ITER_INC, collect_visit,
                      &crt_paths),
            0);
  EXPECT_LT(H5G_visit(&root_loc, "missing", H5_INDEX_NAME, H5_ITER_INC,
                      collect_visit, &crt_paths),
            0);
  auto err_visit = [](hid_t /*g*/, const char* /*path*/,
                      const H5L_info2_t* /*info*/, void* /*d*/) -> herr_t {
    return H5_ITER_ERROR;
  };
  EXPECT_LT(H5Lvisit2(file.get(), H5_INDEX_NAME, H5_ITER_INC, err_visit,
                      nullptr),
            0);
  EXPECT_LT(H5G_iterate(&root_loc, "missing", H5_INDEX_NAME, H5_ITER_INC, 0,
                        &last_lnk, &op_new, &iter_names),
            0);
}

TEST(H5GintTest, GetCreatePlistCoversGinfoLinfoAndPline) {
  ScopedFile file = CreateInMemoryFile("gint_gcpl.h5");
  ASSERT_TRUE(file.valid());
  H5CX_node_t api_ctx{};
  ASSERT_GE(H5CX_push(&api_ctx), 0);
  absl::Cleanup pop_ctx = [] { H5CX_pop(false); };

  // 1. Old-style group (no ginfo, no linfo, no pline).
  ScopedGroup g_old(H5Gcreate2(file.get(), "g_old", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
  ASSERT_TRUE(g_old.valid());
  ScopedPlist gcpl_old(H5G_get_create_plist(UnwrapGroup(g_old)));
  EXPECT_TRUE(gcpl_old.valid());

  // 2. Group with ginfo + linfo + filter pipeline (zlib deflate).
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_local_heap_size_hint(gcpl.get(), 256), 0);
  ASSERT_GE(H5Pset_link_phase_change(gcpl.get(), 12, 6), 0);
  ASSERT_GE(H5Pset_deflate(gcpl.get(), 6), 0);

  ScopedGroup g_full(H5Gcreate2(file.get(), "g_full", H5P_DEFAULT, gcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(g_full.valid());
  ScopedPlist retrieved(H5G_get_create_plist(UnwrapGroup(g_full)));
  ASSERT_TRUE(retrieved.valid());

  int nfilters = H5Pget_nfilters(retrieved.get());
  EXPECT_EQ(nfilters, 1);
  unsigned max_compact = 0;
  unsigned min_dense = 0;
  ASSERT_GE(
      H5Pget_link_phase_change(retrieved.get(), &max_compact, &min_dense), 0);
  EXPECT_EQ(max_compact, 12u);
  EXPECT_EQ(min_dense, 6u);
}

TEST(H5GintTest, TopTermPackageAndTermPackageWithOpenGroup) {
  // Open a group and leave it open across H5close() so H5G_top_term_package()
  // and H5G_term_package() exercise non-empty ID table shutdown, then
  // re-open HDF5 so later tests see an initialized library.
  absl::Cleanup reopen = [] { H5open(); };
  {
    {
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      hid_t fid = H5Fcreate("gint_term.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get());
      ASSERT_NE(fid, H5I_INVALID_HID);
      hid_t gid = H5Gcreate2(fid, "open_grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT);
      ASSERT_NE(gid, H5I_INVALID_HID);
      EXPECT_GE(H5Fclose(fid), 0);
    }

    // Terminate library while gid is still open.
    ASSERT_GE(H5close(), 0);
  }
  ASSERT_GE(H5open(), 0);
}

// ---------------------------------------------------------------------------
// In-memory VFD with injectable read/write/flush failures for metadata I/O.
// ---------------------------------------------------------------------------

bool g_failmem_fail_read = false;
bool g_failmem_fail_write = false;

std::unordered_map<std::string, std::vector<uint8_t>>& FailMemStore() {
  static auto* store =
      new std::unordered_map<std::string, std::vector<uint8_t>>();
  return *store;
}

struct FailMemFile {
  H5FD_t pub{};
  std::string name;
  haddr_t eoa = 0;
  std::vector<uint8_t>* buf = nullptr;
};

H5FD_t* FailMemOpen(const char* name, unsigned flags, hid_t /*fapl_id*/,
                    haddr_t maxaddr) {
  if (name == nullptr || *name == '\0') return nullptr;
  auto& store = FailMemStore();
  if (flags & H5F_ACC_TRUNC) {
    store[name].clear();
  } else if (store.find(name) == store.end()) {
    if (flags & H5F_ACC_CREAT) {
      store[name] = {};
    } else {
      return nullptr;
    }
  }
  auto* f = new FailMemFile();
  f->name = name;
  f->buf = &store[name];
  f->eoa = 0;
  f->pub.maxaddr = maxaddr;
  return &f->pub;
}

herr_t FailMemClose(H5FD_t* file) {
  delete reinterpret_cast<FailMemFile*>(file);
  return 0;
}

int FailMemCmp(const H5FD_t* f1, const H5FD_t* f2) {
  const auto* a = reinterpret_cast<const FailMemFile*>(f1);
  const auto* b = reinterpret_cast<const FailMemFile*>(f2);
  if (a->name < b->name) return -1;
  if (a->name > b->name) return 1;
  return 0;
}

herr_t FailMemQuery(const H5FD_t* /*f*/, unsigned long* flags) {
  if (flags != nullptr) *flags = 0;
  return 0;
}

haddr_t FailMemGetEoa(const H5FD_t* file, H5FD_mem_t /*type*/) {
  return reinterpret_cast<const FailMemFile*>(file)->eoa;
}

herr_t FailMemSetEoa(H5FD_t* file, H5FD_mem_t /*type*/, haddr_t addr) {
  reinterpret_cast<FailMemFile*>(file)->eoa = addr;
  return 0;
}

haddr_t FailMemGetEof(const H5FD_t* file, H5FD_mem_t /*type*/) {
  return reinterpret_cast<const FailMemFile*>(file)->buf->size();
}

herr_t FailMemRead(H5FD_t* file, H5FD_mem_t /*type*/, hid_t /*dxpl*/,
                   haddr_t addr, size_t size, void* buf) {
  if (g_failmem_fail_read) return -1;
  auto* f = reinterpret_cast<FailMemFile*>(file);
  std::memset(buf, 0, size);
  if (addr < f->buf->size()) {
    const size_t avail = static_cast<size_t>(f->buf->size() - addr);
    const size_t to_copy = std::min(size, avail);
    std::memcpy(buf, f->buf->data() + addr, to_copy);
  }
  return 0;
}

herr_t FailMemWrite(H5FD_t* file, H5FD_mem_t /*type*/, hid_t /*dxpl*/,
                    haddr_t addr, size_t size, const void* buf) {
  if (g_failmem_fail_write) return -1;
  auto* f = reinterpret_cast<FailMemFile*>(file);
  if (addr + size > f->buf->size()) {
    f->buf->resize(static_cast<size_t>(addr + size), 0);
  }
  if (size > 0) {
    std::memcpy(f->buf->data() + addr, buf, size);
  }
  return 0;
}

herr_t FailMemFlush(H5FD_t* /*file*/, hid_t /*dxpl*/, bool /*closing*/) {
  return g_failmem_fail_write ? -1 : 0;
}

herr_t FailMemTruncate(H5FD_t* /*file*/, hid_t /*dxpl*/, bool /*closing*/) {
  return 0;
}

const H5FD_class_t kFailMemVfdCls = {
    H5FD_CLASS_VERSION,          // version
    500,                         // value
    "failmem_gint_test",         // name
    (haddr_t{1} << 48),          // maxaddr
    H5F_CLOSE_WEAK,              // fc_degree
    nullptr,                     // terminate
    nullptr,                     // sb_size
    nullptr,                     // sb_encode
    nullptr,                     // sb_decode
    0,                           // fapl_size
    nullptr,                     // fapl_get
    nullptr,                     // fapl_copy
    nullptr,                     // fapl_free
    0,                           // dxpl_size
    nullptr,                     // dxpl_copy
    nullptr,                     // dxpl_free
    FailMemOpen,                 // open
    FailMemClose,                // close
    FailMemCmp,                  // cmp
    FailMemQuery,                // query
    nullptr,                     // get_type_map
    nullptr,                     // alloc
    nullptr,                     // free
    FailMemGetEoa,               // get_eoa
    FailMemSetEoa,               // set_eoa
    FailMemGetEof,               // get_eof
    nullptr,                     // get_handle
    FailMemRead,                 // read
    FailMemWrite,                // write
    nullptr,                     // read_vector
    nullptr,                     // write_vector
    nullptr,                     // read_selection
    nullptr,                     // write_selection
    FailMemFlush,                // flush
    FailMemTruncate,             // truncate
    nullptr,                     // lock
    nullptr,                     // unlock
    nullptr,                     // del
    nullptr,                     // ctl
    H5FD_FLMAP_DICHOTOMY         // fl_map
};

// ---------------------------------------------------------------------------
// Custom VOL wrapper connector with injectable group close/get_object errors.
// ---------------------------------------------------------------------------

bool g_fail_vol_close = false;
bool g_fail_vol_get_object = false;

struct FailWrapObj {
  void* real_obj = nullptr;
  bool closed = false;
};

void* FailVolFileCreate(const char* /*name*/, unsigned /*flags*/,
                        hid_t /*fcpl_id*/, hid_t /*fapl_id*/,
                        hid_t /*dxpl_id*/, void** /*req*/) {
  return new int(1);
}

herr_t FailVolFileClose(void* file, hid_t /*dxpl_id*/, void** /*req*/) {
  delete static_cast<int*>(file);
  return 0;
}

void* FailVolGetObject(const void* obj) {
  const auto* w = static_cast<const FailWrapObj*>(obj);
  return g_fail_vol_get_object ? nullptr : w->real_obj;
}

herr_t FailVolGetWrapCtx(const void* /*obj*/, void** wrap_ctx) {
  *wrap_ctx = new int(2);
  return 0;
}

void* FailVolWrapObject(void* obj, H5I_type_t /*obj_type*/,
                        void* /*wrap_ctx*/) {
  return new FailWrapObj{obj, false};
}

void* FailVolUnwrapObject(void* obj) {
  auto* w = static_cast<FailWrapObj*>(obj);
  void* inner = w->real_obj;
  delete w;
  return inner;
}

herr_t FailVolFreeWrapCtx(void* wrap_ctx) {
  delete static_cast<int*>(wrap_ctx);
  return 0;
}

herr_t FailVolGroupClose(void* grp, hid_t /*dxpl_id*/, void** /*req*/) {
  auto* w = static_cast<FailWrapObj*>(grp);
  if (!w->closed) {
    if (H5G_close(static_cast<H5G_t*>(w->real_obj)) < 0) return -1;
    w->closed = true;
  }
  if (g_fail_vol_close) return -1;
  delete w;
  return 0;
}

herr_t FailVolGetCapFlags(const void* /*info*/, uint64_t* cap_flags) {
  if (cap_flags != nullptr) *cap_flags = 0;
  return 0;
}

herr_t FailVolOptQuery(void* /*obj*/, H5VL_subclass_t /*cls*/,
                       int /*opt_type*/, uint64_t* flags) {
  if (flags != nullptr) *flags = 0;
  return 0;
}

const H5VL_class_t& FailVolClass() {
  static const H5VL_class_t kCls = [] {
    H5VL_class_t cls{};
    cls.version = H5VL_VERSION;
    cls.value = 501;
    cls.name = "failwrap_gint_test";
    cls.wrap_cls.get_object = FailVolGetObject;
    cls.wrap_cls.get_wrap_ctx = FailVolGetWrapCtx;
    cls.wrap_cls.wrap_object = FailVolWrapObject;
    cls.wrap_cls.unwrap_object = FailVolUnwrapObject;
    cls.wrap_cls.free_wrap_ctx = FailVolFreeWrapCtx;
    cls.file_cls.create = FailVolFileCreate;
    cls.file_cls.close = FailVolFileClose;
    cls.group_cls.close = FailVolGroupClose;
    cls.introspect_cls.get_cap_flags = FailVolGetCapFlags;
    cls.introspect_cls.opt_query = FailVolOptQuery;
    return cls;
  }();
  return kCls;
}

// ---------------------------------------------------------------------------
// Custom UD link classes for query failure and H5O_loc_free failure tests.
// ---------------------------------------------------------------------------

constexpr H5L_type_t kUdFailQueryId = static_cast<H5L_type_t>(H5L_TYPE_MAX - 2);
constexpr H5L_type_t kUdExtHoldId = static_cast<H5L_type_t>(H5L_TYPE_MAX - 1);

hid_t UdDummyTraverse(const char* /*name*/, hid_t /*cur_grp*/,
                      const void* /*udata*/, size_t /*size*/,
                      hid_t /*lapl_id*/, hid_t /*dxpl_id*/) {
  return H5I_INVALID_HID;
}

ssize_t UdFailingQuery(const char* /*name*/, const void* /*udata*/,
                       size_t /*size*/, void* /*buf*/, size_t /*buf_size*/) {
  return -1;
}

struct ExtHoldPayload {
  hid_t ext_fid = H5I_INVALID_HID;
  int64_t open_group = 0;
};
static_assert(sizeof(ExtHoldPayload) == sizeof(hid_t) + sizeof(int64_t));

hid_t UdExtHoldTraverse(const char* /*name*/, hid_t /*cur_grp*/,
                        const void* udata, size_t size, hid_t /*lapl_id*/,
                        hid_t /*dxpl_id*/) {
  if (udata == nullptr || size != sizeof(ExtHoldPayload)) {
    return H5I_INVALID_HID;
  }
  ExtHoldPayload payload{};
  std::memcpy(&payload, udata, sizeof(payload));
  if (payload.open_group != 0) {
    return H5Gopen2(payload.ext_fid, "ext_g", H5P_DEFAULT);
  }
  return H5Dopen2(payload.ext_fid, "ext_dset", H5P_DEFAULT);
}

TEST(H5GintTest, IterateAndVisitFailingQueryEarlyStopErrorAndOldFallback) {
  // 1. Failing H5G_link_to_info inside H5G__iterate_cb & H5G__visit_cb via UD
  //    link with failing query_func.
  const H5L_class_t fail_query_cls = {
      H5L_LINK_CLASS_T_VERS, kUdFailQueryId, "ud_fail_query_gint",
      nullptr, nullptr, nullptr, UdDummyTraverse, nullptr, UdFailingQuery};
  ASSERT_GE(H5Lregister(&fail_query_cls), 0);
  absl::Cleanup unreg_fq = [] {
    if (H5Lis_registered(kUdFailQueryId) > 0) H5Lunregister(kUdFailQueryId);
  };

  {
    ScopedFile f = CreateInMemoryFile("gint_ud_fq.h5");
    ASSERT_TRUE(f.valid());
    const uint32_t dummy = 1;
    ASSERT_GE(H5Lcreate_ud(f.get(), "bad_ud", kUdFailQueryId, &dummy,
                           sizeof(dummy), H5P_DEFAULT, H5P_DEFAULT),
              0);

    ScopedErrorSilence silence;
    hsize_t idx = 0;
    auto iter_cb = [](hid_t /*g*/, const char* /*name*/,
                      const H5L_info2_t* /*info*/, void* /*d*/) -> herr_t {
      return H5_ITER_CONT;
    };
    EXPECT_LT(H5Literate2(f.get(), H5_INDEX_NAME, H5_ITER_INC, &idx, iter_cb,
                          nullptr),
              0);
    EXPECT_LT(H5Lvisit2(f.get(), H5_INDEX_NAME, H5_ITER_INC, iter_cb, nullptr),
              0);
  }

  // 2. Callback returning H5_ITER_ERROR in H5G_iterate + deleting compact link
  //    inside H5Lvisit2 callback before H5G_loc_find recursion.
  {
    ScopedFile f = CreateInMemoryFile("gint_iter_err_del.h5");
    ASSERT_TRUE(f.valid());
    ScopedGroup ga(
        H5Gcreate2(f.get(), "a", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ga.valid());
    ga.reset();

    ScopedErrorSilence silence;
    hsize_t idx = 0;
    auto err_iter_cb = [](hid_t /*g*/, const char* /*name*/,
                          const H5L_info2_t* /*info*/, void* /*d*/) -> herr_t {
      return H5_ITER_ERROR;
    };
    EXPECT_LT(H5Literate2(f.get(), H5_INDEX_NAME, H5_ITER_INC, &idx,
                          err_iter_cb, nullptr),
              0);

    auto del_during_visit = [](hid_t g, const char* path,
                               const H5L_info2_t* /*info*/,
                               void* /*d*/) -> herr_t {
      (void)H5Ldelete(g, path, H5P_DEFAULT);
      return H5_ITER_CONT;
    };
    EXPECT_LT(H5Lvisit2(f.get(), H5_INDEX_NAME, H5_ITER_INC, del_during_visit,
                        nullptr),
              0);
  }

  // 3. Old-format symbol-table file (H5F_LIBVER_EARLIEST) visited with
  //    H5_INDEX_CRT_ORDER hits both top-level and recursive !linfo_exists
  //    fallbacks (H5G_visit and H5G__visit_cb).
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_LATEST),
              0);
    ScopedFile f(H5Fcreate("gint_earliest.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                           fapl.get()));
    ASSERT_TRUE(f.valid());
    ScopedGroup old_g(
        H5Gcreate2(f.get(), "old_stab", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(old_g.valid());
    ScopedGroup child(H5Gcreate2(old_g.get(), "child", H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
    ASSERT_TRUE(child.valid());
    child.reset();
    old_g.reset();

    std::vector<std::string> visited;
    auto collect = [](hid_t /*g*/, const char* path,
                      const H5L_info2_t* /*info*/, void* d) -> herr_t {
      static_cast<std::vector<std::string>*>(d)->emplace_back(path);
      return H5_ITER_CONT;
    };
    ASSERT_GE(H5Lvisit2(f.get(), H5_INDEX_CRT_ORDER, H5_ITER_INC, collect,
                        &visited),
              0);
    EXPECT_THAT(visited, ElementsAre("old_stab", "old_stab/child"));
  }

  // 4. H5CX_push without vol_wrap_ctx causes H5VL_wrap_register failure in
  //    H5G_iterate and H5G_visit; H5G_get_create_plist on bad oloc cleans up
  //    copied gcpl ID.
  {
    ScopedFile f = CreateInMemoryFile("gint_nowrap_ctx.h5");
    ASSERT_TRUE(f.valid());

    H5CX_node_t api_ctx{};
    ASSERT_GE(H5CX_push(&api_ctx), 0);
    absl::Cleanup pop_ctx = [] { H5CX_pop(false); };

    H5G_loc_t root_loc{};
    ASSERT_GE(H5G_loc(f.get(), &root_loc), 0);
    auto dummy_iter = [](hid_t /*g*/, const char* /*name*/,
                         const H5L_info2_t* /*info*/, void* /*d*/) -> herr_t {
      return H5_ITER_CONT;
    };
    H5G_link_iterate_t op{};
    op.op_type = H5G_LINK_OP_NEW;
    op.op_func.op_new = dummy_iter;
    hsize_t last = 0;

    ScopedErrorSilence silence;
    EXPECT_LT(H5G_iterate(&root_loc, ".", H5_INDEX_NAME, H5_ITER_INC, 0, &last,
                          &op, nullptr),
              0);
    EXPECT_LT(H5G_visit(&root_loc, ".", H5_INDEX_NAME, H5_ITER_INC, dummy_iter,
                        nullptr),
              0);

    H5G_t bad_grp{};
    bad_grp.oloc.file = UnwrapFile(f);
    bad_grp.oloc.addr = HADDR_UNDEF;
    EXPECT_EQ(H5G_get_create_plist(&bad_grp), H5I_INVALID_HID);
  }
}

TEST(H5GintTest, CorruptedMessagesFailDecodeAndDenseLinkInfo) {
  hid_t vfd_id = H5FDregister(&kFailMemVfdCls);
  ASSERT_GE(vfd_id, 0);
  absl::Cleanup unreg_vfd = [&] {
    g_failmem_fail_read = false;
    g_failmem_fail_write = false;
    FailMemStore().clear();
    H5FDunregister(vfd_id);
  };

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_driver(fapl.get(), vfd_id, nullptr), 0);

  const char* kFname = "corrupt_msgs_gint.h5";
  {
    ScopedFile f(
        H5Fcreate(kFname, H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());

    ScopedPlist gcpl1(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl1.valid());
    ASSERT_GE(H5Pset_link_phase_change(gcpl1.get(), 4, 2), 0);
    ScopedGroup g1(
        H5Gcreate2(f.get(), "g_ginfo", H5P_DEFAULT, gcpl1.get(), H5P_DEFAULT));
    ASSERT_TRUE(g1.valid());

    ScopedPlist gcpl2(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl2.valid());
    ASSERT_GE(H5Pset_deflate(gcpl2.get(), 5), 0);
    ScopedGroup g2(
        H5Gcreate2(f.get(), "g_pline", H5P_DEFAULT, gcpl2.get(), H5P_DEFAULT));
    ASSERT_TRUE(g2.valid());

    ScopedPlist gcpl3(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl3.valid());
    ASSERT_GE(H5Pset_link_phase_change(gcpl3.get(), 0, 0), 0);
    ScopedGroup g3(
        H5Gcreate2(f.get(), "dense_g", H5P_DEFAULT, gcpl3.get(), H5P_DEFAULT));
    ASSERT_TRUE(g3.valid());
    ScopedGroup item(
        H5Gcreate2(g3.get(), "item", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(item.valid());
  }

  ScopedFile f(H5Fopen(kFname, H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(f.valid());
  H5CX_node_t api_ctx{};
  ASSERT_GE(H5CX_push(&api_ctx), 0);
  absl::Cleanup pop_ctx = [] { H5CX_pop(false); };

  // Helper to flip/restore the first raw byte of an undecoded header message.
  auto test_bad_raw_decode = [&](const char* grp_name, unsigned msg_id) {
    ScopedGroup grp(H5Gopen2(f.get(), grp_name, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    H5G_t* g = UnwrapGroup(grp);
    H5O_t* oh = H5O_protect(&g->oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    int idx = -1;
    for (size_t i = 0; i < oh->nmesgs; ++i) {
      if (oh->mesg[i].type && oh->mesg[i].type->id == msg_id) {
        idx = static_cast<int>(i);
        break;
      }
    }
    ASSERT_GE(idx, 0);
    ASSERT_EQ(oh->mesg[idx].native, nullptr);
    ASSERT_GT(oh->mesg[idx].raw_size, 0u);
    const uint8_t saved = oh->mesg[idx].raw[0];
    oh->mesg[idx].raw[0] = 99;  // Invalid message version.
    ASSERT_GE(H5O_unprotect(&g->oloc, oh, H5AC__NO_FLAGS_SET), 0);

    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5G_get_create_plist(g), H5I_INVALID_HID);
    }

    oh = H5O_protect(&g->oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    oh->mesg[idx].raw[0] = saved;
    ASSERT_GE(H5O_unprotect(&g->oloc, oh, H5AC__NO_FLAGS_SET), 0);
  };

  test_bad_raw_decode("g_ginfo", H5O_GINFO_ID);
  test_bad_raw_decode("g_pline", H5O_PLINE_ID);

  // Corrupt dense group's native LINFO message so H5G__obj_get_linfo fails in
  // H5G_get_create_plist, H5G_visit, and H5G__visit_cb.
  {
    ScopedGroup dg(H5Gopen2(f.get(), "dense_g", H5P_DEFAULT));
    ASSERT_TRUE(dg.valid());
    H5G_t* g = UnwrapGroup(dg);
    H5O_linfo_t tmp_linfo{};
    ASSERT_NE(H5O_msg_read(&g->oloc, H5O_LINFO_ID, &tmp_linfo), nullptr);

    H5O_t* oh = H5O_protect(&g->oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    H5O_linfo_t* linfo = nullptr;
    for (size_t i = 0; i < oh->nmesgs; ++i) {
      if (oh->mesg[i].type && oh->mesg[i].type->id == H5O_LINFO_ID) {
        linfo = static_cast<H5O_linfo_t*>(oh->mesg[i].native);
        break;
      }
    }
    ASSERT_NE(linfo, nullptr);
    const hsize_t saved_nlinks = linfo->nlinks;
    const haddr_t saved_bt2 = linfo->name_bt2_addr;
    linfo->nlinks = HSIZET_MAX;
    linfo->name_bt2_addr = HADDR_UNDEF;
    ASSERT_GE(H5O_unprotect(&g->oloc, oh, H5AC__NO_FLAGS_SET), 0);

    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5G_get_create_plist(g), H5I_INVALID_HID);
      auto noop = [](hid_t /*g*/, const char* /*p*/,
                     const H5L_info2_t* /*info*/, void* /*d*/) -> herr_t {
        return H5_ITER_CONT;
      };
      EXPECT_LT(H5Lvisit_by_name2(f.get(), "dense_g", H5_INDEX_NAME,
                                  H5_ITER_INC, noop, nullptr, H5P_DEFAULT),
                0);
      EXPECT_LT(
          H5Lvisit2(f.get(), H5_INDEX_NAME, H5_ITER_INC, noop, nullptr), 0);
    }

    oh = H5O_protect(&g->oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    linfo->nlinks = saved_nlinks;
    linfo->name_bt2_addr = saved_bt2;
    ASSERT_GE(H5O_unprotect(&g->oloc, oh, H5AC__NO_FLAGS_SET), 0);
  }
}

TEST(H5GintTest, CloseErrorPathsEvictMountAndLocFree) {
  H5CX_node_t api_ctx{};
  ASSERT_GE(H5CX_push(&api_ctx), 0);
  absl::Cleanup pop_ctx = [] { H5CX_pop(false); };

  // 1. Synthetic H5FO_top_decr / H5FO_delete failures in H5G_close.
  {
    ScopedFile f = CreateInMemoryFile("gint_fo_err.h5");
    ASSERT_TRUE(f.valid());
    H5F_t* f_ptr = UnwrapFile(f);
    H5G_shared_t fake_shared{};
    H5G_t fake_grp{};
    fake_grp.oloc.file = f_ptr;
    fake_grp.oloc.addr = 0xdeadbeefULL;
    fake_grp.shared = &fake_shared;

    ScopedErrorSilence silence;
    fake_shared.fo_count = 1;
    EXPECT_LT(H5G_close(&fake_grp), 0);

    ASSERT_GE(H5FO_top_incr(f_ptr, 0xdeadbeefULL), 0);
    fake_shared.fo_count = 1;
    EXPECT_LT(H5G_close(&fake_grp), 0);

    fake_shared.fo_count = 2;
    EXPECT_LT(H5G_close(&fake_grp), 0);
  }

  // 2. H5O_close failures (fo_count == 1, fo_count > 1 top_count == 0, and
  //    mounted && fo_count == 1) triggered safely with H5F_CLOSE_DEFAULT after
  //    closing the user file ID on a WEAK file.
  {
    ScopedFile f = CreateInMemoryFile("gint_close_def1.h5");
    ASSERT_TRUE(f.valid());
    ScopedGroup tmp_g(
        H5Gcreate2(f.get(), "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(tmp_g.valid());
    tmp_g.reset();

    H5G_loc_t loc{};
    ASSERT_GE(H5G_loc(f.get(), &loc), 0);
    H5G_t* g = H5G__open_name(&loc, "g");
    ASSERT_NE(g, nullptr);
    H5F_t* f_ptr = g->oloc.file;
    f.reset();

    f_ptr->shared->fc_degree = H5F_CLOSE_DEFAULT;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5G_close(g), 0);
    }
    f_ptr->shared->fc_degree = H5F_CLOSE_WEAK;
    EXPECT_GE(H5G_name_free(&g->path), 0);
    H5FL_FREE(H5G_shared_t, g->shared);
    H5FL_FREE(H5G_t, g);
    EXPECT_GE(H5F_try_close(f_ptr, nullptr), 0);
  }

  {
    ScopedFile f1 = CreateInMemoryFile("gint_close_def2.h5");
    ASSERT_TRUE(f1.valid());
    ScopedGroup g1(
        H5Gcreate2(f1.get(), "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g1.valid());

    ScopedFile f2(H5Freopen(f1.get()));
    ASSERT_TRUE(f2.valid());
    H5G_loc_t loc2{};
    ASSERT_GE(H5G_loc(f2.get(), &loc2), 0);
    H5G_t* g2 = H5G__open_name(&loc2, "g");
    ASSERT_NE(g2, nullptr);
    H5F_t* f2_ptr = g2->oloc.file;
    f2.reset();

    f2_ptr->shared->fc_degree = H5F_CLOSE_DEFAULT;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5G_close(g2), 0);
    }
    f2_ptr->shared->fc_degree = H5F_CLOSE_WEAK;
    EXPECT_GE(H5G_name_free(&g2->path), 0);
    H5FL_FREE(H5G_t, g2);
    EXPECT_GE(H5F_try_close(f2_ptr, nullptr), 0);
  }

  {
    ScopedFile parent = CreateInMemoryFile("gint_close_def3_p.h5");
    ASSERT_TRUE(parent.valid());
    ScopedGroup tmp_mp(
        H5Gcreate2(parent.get(), "mp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(tmp_mp.valid());
    tmp_mp.reset();

    H5G_loc_t p_loc{};
    ASSERT_GE(H5G_loc(parent.get(), &p_loc), 0);
    H5G_t* mp = H5G__open_name(&p_loc, "mp");
    ASSERT_NE(mp, nullptr);
    H5F_t* p_ptr = mp->oloc.file;

    ScopedFile child = CreateInMemoryFile("gint_close_def3_c.h5");
    ASSERT_TRUE(child.valid());
    ASSERT_GE(H5Fmount(parent.get(), "mp", child.get(), H5P_DEFAULT), 0);
    child.reset();
    parent.reset();

    p_ptr->shared->fc_degree = H5F_CLOSE_DEFAULT;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5G_close(mp), 0);
    }
    p_ptr->shared->fc_degree = H5F_CLOSE_WEAK;
    EXPECT_GE(H5G_name_free(&mp->path), 0);
    H5FL_FREE(H5G_t, mp);
    EXPECT_GE(H5F_try_close(p_ptr, nullptr), 0);
  }

  // 3. evict_on_close flush failure and pinned eviction failure in H5G_close.
  {
    hid_t vfd_id = H5FDregister(&kFailMemVfdCls);
    ASSERT_GE(vfd_id, 0);
    absl::Cleanup unreg_vfd = [&] {
      g_failmem_fail_read = false;
      g_failmem_fail_write = false;
      FailMemStore().clear();
      H5FDunregister(vfd_id);
    };

    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_driver(fapl.get(), vfd_id, nullptr), 0);
    ASSERT_GE(H5Pset_evict_on_close(fapl.get(), true), 0);

    ScopedFile f(
        H5Fcreate("gint_evict_err.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    ScopedGroup tmp_g(
        H5Gcreate2(f.get(), "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(tmp_g.valid());
    tmp_g.reset();
    ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);

    H5G_loc_t f_loc{};
    ASSERT_GE(H5G_loc(f.get(), &f_loc), 0);
    H5G_t* g = H5G__open_name(&f_loc, "g");
    ASSERT_NE(g, nullptr);
    // Modify "g" so tagged metadata under g's ohdr addr is dirty.
    ScopedGroup dirty_sub(
        H5Gcreate2(f.get(), "g/dirty", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dirty_sub.valid());
    dirty_sub.reset();

    g_failmem_fail_write = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5G_close(g), 0);
    }
    g_failmem_fail_write = false;
    EXPECT_GE(H5G_name_free(&g->path), 0);
    H5FL_FREE(H5G_shared_t, g->shared);
    H5FL_FREE(H5G_t, g);

    // H5C_flush_tagged_entries left slist enabled on flush failure; finish
    // flushing the skip list and disable it before subsequent operations.
    H5F_t* f_ptr = UnwrapFile(f);
    ASSERT_NE(f_ptr, nullptr);
    ASSERT_GE(H5C_flush_cache(f_ptr, H5C__FLUSH_IGNORE_PROTECTED_FLAG), 0);
    ASSERT_GE(H5C_set_slist_enabled(f_ptr->shared->cache, false, false), 0);

    ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);
    H5G_t* g2 = H5G__open_name(&f_loc, "g");
    ASSERT_NE(g2, nullptr);
    H5O_t* oh = H5O_protect(&g2->oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    ASSERT_GE(H5AC_pin_protected_entry(oh), 0);
    ASSERT_GE(H5O_unprotect(&g2->oloc, oh, H5AC__NO_FLAGS_SET), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5G_close(g2), 0);
    }
    ASSERT_GE(H5AC_unpin_entry(oh), 0);
    EXPECT_GE(H5G_name_free(&g2->path), 0);
    H5FL_FREE(H5G_shared_t, g2->shared);
    H5FL_FREE(H5G_t, g2);
  }

  // 4. H5O_loc_free failure paths in H5G__open_name, H5G__get_info_by_name,
  //    H5G__get_info_by_idx, and H5G_close (else branch) via custom UD link.
  {
    const H5L_class_t ext_hold_cls = {
        H5L_LINK_CLASS_T_VERS, kUdExtHoldId, "ud_ext_hold_gint",
        nullptr, nullptr, nullptr, UdExtHoldTraverse, nullptr, nullptr};
    ASSERT_GE(H5Lregister(&ext_hold_cls), 0);
    absl::Cleanup unreg_ud = [] {
      if (H5Lis_registered(kUdExtHoldId) > 0) H5Lunregister(kUdExtHoldId);
    };

    ScopedFile f_ext = CreateInMemoryFile("gint_ext_target.h5");
    ASSERT_TRUE(f_ext.valid());
    ScopedSpace space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(f_ext.get(), "ext_dset", H5T_NATIVE_INT,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    dset.reset();
    ScopedGroup ext_g(H5Gcreate2(f_ext.get(), "ext_g", H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
    ASSERT_TRUE(ext_g.valid());
    ext_g.reset();

    ScopedFile f_host = CreateInMemoryFile("gint_ext_host.h5");
    ASSERT_TRUE(f_host.valid());
    const ExtHoldPayload p_dset{f_ext.get(), 0};
    ASSERT_GE(H5Lcreate_ud(f_host.get(), "a_ud_dset", kUdExtHoldId, &p_dset,
                           sizeof(p_dset), H5P_DEFAULT, H5P_DEFAULT),
              0);
    const ExtHoldPayload p_grp{f_ext.get(), 1};
    ASSERT_GE(H5Lcreate_ud(f_host.get(), "b_ud_grp", kUdExtHoldId, &p_grp,
                           sizeof(p_grp), H5P_DEFAULT, H5P_DEFAULT),
              0);

    H5F_t* ext_ptr = UnwrapFile(f_ext);
    absl::Cleanup restore_deg = [&] {
      ext_ptr->shared->fc_degree = H5F_CLOSE_WEAK;
    };

    H5G_loc_t host_loc{};
    ASSERT_GE(H5G_loc(f_host.get(), &host_loc), 0);
    H5VL_object_t* host_vol = H5VL_vol_object(f_host.get());
    ASSERT_NE(host_vol, nullptr);
    ASSERT_GE(H5VL_set_vol_wrapper(host_vol), 0);
    absl::Cleanup reset_wrap = [] { H5VL_reset_vol_wrapper(); };

    // Hit H5G_close else-branch cleanly via UD link + H5FO_top_incr.
    {
      ScopedFile f_ext2(H5Freopen(f_ext.get()));
      ASSERT_TRUE(f_ext2.valid());
      ScopedGroup g_keep(H5Gopen2(f_ext2.get(), "ext_g", H5P_DEFAULT));
      ASSERT_TRUE(g_keep.valid());

      H5G_t* g_ud = H5G__open_name(&host_loc, "b_ud_grp");
      ASSERT_NE(g_ud, nullptr);
      ASSERT_GE(H5FO_top_incr(ext_ptr, g_ud->oloc.addr), 0);
      // H5O_open cleared holding_file while ext_ptr->nopen_objs == 1; temporarily
      // emulate holding_file == true so H5O_loc_free drops nopen_objs to 0 and
      // hits H5F_try_close failure on fc_degree == H5F_CLOSE_DEFAULT.
      const bool saved_hold = g_ud->oloc.holding_file;
      const unsigned saved_nopen = ext_ptr->nopen_objs;
      g_ud->oloc.holding_file = true;
      ext_ptr->nopen_objs = 1;
      ext_ptr->shared->fc_degree = H5F_CLOSE_DEFAULT;
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5G_close(g_ud), 0);
      }
      ext_ptr->shared->fc_degree = H5F_CLOSE_WEAK;
      g_ud->oloc.holding_file = saved_hold;
      ext_ptr->nopen_objs = saved_nopen;
      EXPECT_GE(H5FO_top_decr(ext_ptr, g_ud->oloc.addr), 0);
      EXPECT_GE(H5O_close(&g_ud->oloc, nullptr), 0);
      EXPECT_GE(H5G_name_free(&g_ud->path), 0);
      H5FL_FREE(H5G_t, g_ud);
    }

    ext_ptr->shared->fc_degree = H5F_CLOSE_DEFAULT;
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5G__open_name(&host_loc, "a_ud_dset"), nullptr);
      H5G_info_t ginfo{};
      EXPECT_LT(H5G__get_info_by_name(&host_loc, "a_ud_dset", &ginfo), 0);
      EXPECT_LT(H5G__get_info_by_idx(&host_loc, ".", H5_INDEX_NAME, H5_ITER_INC,
                                     0, &ginfo),
                0);
    }
    ext_ptr->shared->fc_degree = H5F_CLOSE_WEAK;
  }
}

TEST(H5GintTest, FailMemReadAndFailWrapVolPaths) {
  // 1. Metadata read errors inside H5G__open_name and H5G__visit_cb.
  {
    hid_t vfd_id = H5FDregister(&kFailMemVfdCls);
    ASSERT_GE(vfd_id, 0);
    absl::Cleanup unreg_vfd = [&] {
      g_failmem_fail_read = false;
      g_failmem_fail_write = false;
      FailMemStore().clear();
      H5FDunregister(vfd_id);
    };

    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_driver(fapl.get(), vfd_id, nullptr), 0);

    const char* kFname = "read_err_gint.h5";
    {
      ScopedFile f(H5Fcreate(kFname, H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
      ScopedGroup cached(
          H5Gcreate2(f.get(), "cached", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(cached.valid());
      ScopedGroup sub(
          H5Gcreate2(f.get(), "sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(sub.valid());
    }

    {
      ScopedFile f(H5Fopen(kFname, H5F_ACC_RDONLY, fapl.get()));
      ASSERT_TRUE(f.valid());
      H5CX_node_t api_ctx{};
      ASSERT_GE(H5CX_push(&api_ctx), 0);
      absl::Cleanup pop_ctx = [] { H5CX_pop(false); };
      H5G_loc_t root_loc{};
      ASSERT_GE(H5G_loc(f.get(), &root_loc), 0);

      // Fail in H5G_loc_find before root link metadata is cached.
      g_failmem_fail_read = true;
      {
        ScopedErrorSilence silence;
        EXPECT_EQ(H5G__open_name(&root_loc, "sub"), nullptr);
      }
      g_failmem_fail_read = false;

      // Warm root link table in cache via "cached", then fail in H5O_obj_type
      // while loading "sub"'s object header.
      {
        ScopedGroup cached(H5Gopen2(f.get(), "cached", H5P_DEFAULT));
        ASSERT_TRUE(cached.valid());
      }
      g_failmem_fail_read = true;
      {
        ScopedErrorSilence silence;
        EXPECT_EQ(H5G__open_name(&root_loc, "sub"), nullptr);
      }
      g_failmem_fail_read = false;
    }

    {
      ScopedFile f(H5Fopen(kFname, H5F_ACC_RDONLY, fapl.get()));
      ASSERT_TRUE(f.valid());
      auto fail_after_find_cb = [](hid_t /*g*/, const char* /*path*/,
                                   const H5L_info2_t* /*info*/,
                                   void* /*d*/) -> herr_t {
        g_failmem_fail_read = true;
        return H5_ITER_CONT;
      };
      ScopedErrorSilence silence;
      EXPECT_LT(H5Lvisit2(f.get(), H5_INDEX_NAME, H5_ITER_INC,
                          fail_after_find_cb, nullptr),
                0);
      g_failmem_fail_read = false;
    }
  }

  // 2. VOL wrapper get_object failure (H5G_visit) and VOL group close failure
  //    (H5G__close_cb, H5G_iterate, H5G_visit).
  {
    const H5VL_class_t vol_cls = FailVolClass();
    hid_t vol_id = H5VLregister_connector(&vol_cls, H5P_DEFAULT);
    ASSERT_GE(vol_id, 0);
    absl::Cleanup unreg_vol = [&] {
      g_fail_vol_close = false;
      g_fail_vol_get_object = false;
      H5VLunregister_connector(vol_id);
    };

    ScopedPlist vol_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(vol_fapl.valid());
    ASSERT_GE(H5Pset_vol(vol_fapl.get(), vol_id, nullptr), 0);
    ScopedFile wrap_f(H5Fcreate("wrap_dummy.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                vol_fapl.get()));
    ASSERT_TRUE(wrap_f.valid());

    ScopedFile real_f = CreateInMemoryFile("wrap_real.h5");
    ASSERT_TRUE(real_f.valid());
    ScopedGroup sub(H5Gcreate2(real_f.get(), "g", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(sub.valid());
    sub.reset();

    H5G_loc_t real_root{};
    ASSERT_GE(H5G_loc(real_f.get(), &real_root), 0);

    H5CX_node_t api_ctx{};
    ASSERT_GE(H5CX_push(&api_ctx), 0);
    absl::Cleanup pop_ctx = [] { H5CX_pop(false); };

    H5VL_object_t* wrap_vol_obj = H5VL_vol_object(wrap_f.get());
    ASSERT_NE(wrap_vol_obj, nullptr);
    ASSERT_GE(H5VL_set_vol_wrapper(wrap_vol_obj), 0);
    absl::Cleanup reset_wrap = [] { H5VL_reset_vol_wrapper(); };

    // Case A: H5G_loc fails because wrap_cls.get_object returns nullptr.
    g_fail_vol_get_object = true;
    auto dummy_visit = [](hid_t /*g*/, const char* /*path*/,
                          const H5L_info2_t* /*info*/, void* /*d*/) -> herr_t {
      return H5_ITER_STOP;
    };
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5G_visit(&real_root, ".", H5_INDEX_NAME, H5_ITER_INC,
                          dummy_visit, nullptr),
                0);
    }
    g_fail_vol_get_object = false;

    // Case B: VOL group close failure in H5G_iterate and H5G_visit.
    hid_t captured_gid = H5I_INVALID_HID;
    auto capture_stop = [](hid_t g, const char* /*name_or_path*/,
                           const H5L_info2_t* /*info*/, void* d) -> herr_t {
      *static_cast<hid_t*>(d) = g;
      return H5_ITER_STOP;
    };

    H5G_link_iterate_t op{};
    op.op_type = H5G_LINK_OP_NEW;
    op.op_func.op_new = capture_stop;
    hsize_t last = 0;

    g_fail_vol_close = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5G_iterate(&real_root, ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                            &last, &op, &captured_gid),
                0);
    }
    g_fail_vol_close = false;
    ASSERT_NE(captured_gid, H5I_INVALID_HID);
    EXPECT_GE(H5Gclose(captured_gid), 0);

    captured_gid = H5I_INVALID_HID;
    g_fail_vol_close = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5G_visit(&real_root, ".", H5_INDEX_NAME, H5_ITER_INC,
                          capture_stop, &captured_gid),
                0);
    }
    g_fail_vol_close = false;
    ASSERT_NE(captured_gid, H5I_INVALID_HID);
    EXPECT_GE(H5Gclose(captured_gid), 0);
  }
}

}  // namespace
}  // namespace hdf5
