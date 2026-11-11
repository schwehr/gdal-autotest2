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

// Unit tests for //third_party/netcdf/libdispatch/ncindex.c.
//
// Exercises NCindex creation, insertion, indexed/name lookup, pointer search,
// sparse slot gaps, element deletion, cloning, rebuilding, debug printing, and
// ncindexverify() consistency/mismatch reporting.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/ncindex.c

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

extern "C" {
#include "third_party/netcdf/include/nc4internal.h"
#include "third_party/netcdf/include/nchashmap.h"
#include "third_party/netcdf/include/ncindex.h"
#include "third_party/netcdf/include/nclist.h"

void printindexlist(NClist* lm);
void printindexmap(NCindex* lm);
void printindex(NCindex* lm);
}  // extern "C"

namespace netcdf {
namespace {

static_assert(offsetof(NC_OBJ, name) >= sizeof(const char*),
              "NC_OBJ::name must sit after initial pointer-sized slot");

struct NcIndexDeleter {
  void operator()(NCindex* idx) const { ncindexfree(idx); }
};
using ScopedNcIndex = std::unique_ptr<NCindex, NcIndexDeleter>;

struct NcListDeleter {
  void operator()(NClist* list) const { nclistfree(list); }
};
using ScopedNcList = std::unique_ptr<NClist, NcListDeleter>;

struct OwnedNcObj {
  std::string name;
  NC_OBJ obj{};

  OwnedNcObj(NC_SORT sort, std::string obj_name, int id)
      : name(std::move(obj_name)) {
    obj.sort = sort;
    obj.name = name.data();
    obj.id = id;
  }

  void Rename(std::string new_name) {
    name = std::move(new_name);
    obj.name = name.data();
  }
};

// Helper for ncindexverify(): upstream ncindexverify() casts list entries to
// `(const char**)` (reading bytes [0..sizeof(void*)-1]) and keystr() returns
// `e->key` only when `keysize >= sizeof(uintptr_t)`. Placing the name pointer
// in both offset 0 and NC_OBJ::name with names >= 8 bytes avoids reading
// uninitialized padding or invalid pointers.
struct VerifyShimObj {
  std::string name;
  std::unique_ptr<NC_OBJ, decltype(&std::free)> raw{
      static_cast<NC_OBJ*>(std::calloc(1, sizeof(NC_OBJ))), &std::free};

  explicit VerifyShimObj(std::string obj_name, int id)
      : name(std::move(obj_name)) {
    SyncPointers(id);
  }

  void SyncPointers(int id) {
    if (raw == nullptr) {
      return;
    }
    char* ptr = name.data();
    std::memcpy(raw.get(), &ptr, sizeof(ptr));
    raw->name = ptr;
    raw->id = id;
  }

  void Rename(std::string new_name) {
    name = std::move(new_name);
    SyncPointers(raw->id);
  }

  NC_OBJ* get() { return raw.get(); }
};

TEST(NcindexTest, NewFreeAndNullGuardsHandleEdgeCases) {
  EXPECT_EQ(1, ncindexfree(nullptr));

  NC_OBJ dummy{NCVAR, nullptr, 0};
  EXPECT_EQ(nullptr, ncindexlookup(nullptr, "var"));
  EXPECT_EQ(nullptr, ncindexith(nullptr, 0));
  EXPECT_EQ(-1, ncindexfind(nullptr, &dummy));
  EXPECT_EQ(0, ncindexadd(nullptr, &dummy));
  EXPECT_EQ(0, ncindexset(nullptr, 0, &dummy));
  EXPECT_EQ(0, ncindexidel(nullptr, 0));
  EXPECT_EQ(nullptr, ncindexdup(nullptr));
  EXPECT_EQ(size_t{0}, ncindexsize(static_cast<NCindex*>(nullptr)));
  EXPECT_FALSE(ncindexinitialized(static_cast<NCindex*>(nullptr)));

  ScopedNcIndex def_idx(ncindexnew(0));
  ASSERT_NE(nullptr, def_idx.get());
  EXPECT_TRUE(ncindexinitialized(def_idx.get()));
  EXPECT_EQ(size_t{0}, ncindexsize(def_idx.get()));
  EXPECT_EQ(0, ncindexcount(def_idx.get()));
  EXPECT_EQ(nullptr, ncindexlookup(def_idx.get(), nullptr));
  EXPECT_EQ(nullptr, ncindexlookup(def_idx.get(), "missing"));
  EXPECT_EQ(-1, ncindexfind(def_idx.get(), nullptr));
  EXPECT_EQ(nullptr, ncindexdup(def_idx.get()));

  ScopedNcIndex sized_idx(ncindexnew(64));
  ASSERT_NE(nullptr, sized_idx.get());
  EXPECT_TRUE(ncindexinitialized(sized_idx.get()));
}

TEST(NcindexTest, AddLookupIthFindAndDupWorkTogether) {
  ScopedNcIndex idx(ncindexnew(0));
  ASSERT_NE(nullptr, idx.get());

  OwnedNcObj a(NCVAR, "temperature", 10);
  OwnedNcObj b(NCDIM, "latitude", 20);
  OwnedNcObj c(NCATT, "units", 30);
  OwnedNcObj unadded(NCTYP, "unadded", 40);

  EXPECT_EQ(1, ncindexadd(idx.get(), &a.obj));
  EXPECT_EQ(1, ncindexadd(idx.get(), &b.obj));
  EXPECT_EQ(1, ncindexadd(idx.get(), &c.obj));

  EXPECT_EQ(3U, ncindexsize(idx.get()));
  EXPECT_EQ(3, ncindexcount(idx.get()));

  EXPECT_EQ(&a.obj, ncindexlookup(idx.get(), "temperature"));
  EXPECT_EQ(&b.obj, ncindexlookup(idx.get(), "latitude"));
  EXPECT_EQ(&c.obj, ncindexlookup(idx.get(), "units"));
  EXPECT_EQ(nullptr, ncindexlookup(idx.get(), "nonexistent"));
  EXPECT_EQ(nullptr, ncindexlookup(idx.get(), ""));

  EXPECT_EQ(&a.obj, ncindexith(idx.get(), 0));
  EXPECT_EQ(&b.obj, ncindexith(idx.get(), 1));
  EXPECT_EQ(&c.obj, ncindexith(idx.get(), 2));
  EXPECT_EQ(nullptr, ncindexith(idx.get(), 3));

  EXPECT_EQ(0, ncindexfind(idx.get(), &a.obj));
  EXPECT_EQ(1, ncindexfind(idx.get(), &b.obj));
  EXPECT_EQ(2, ncindexfind(idx.get(), &c.obj));
  EXPECT_EQ(-1, ncindexfind(idx.get(), &unadded.obj));

  ScopedNcList dup(reinterpret_cast<NClist*>(ncindexdup(idx.get())));
  ASSERT_NE(nullptr, dup.get());
  ASSERT_EQ(3U, nclistlength(dup.get()));
  EXPECT_EQ(&a.obj, nclistget(dup.get(), 0));
  EXPECT_EQ(&b.obj, nclistget(dup.get(), 1));
  EXPECT_EQ(&c.obj, nclistget(dup.get(), 2));
}

TEST(NcindexTest, SetGapsCountIdelAndRebuildMaintainConsistency) {
  ScopedNcIndex idx(ncindexnew(4));
  ASSERT_NE(nullptr, idx.get());

  // Rebuilding an empty index succeeds.
  EXPECT_EQ(1, ncindexrebuild(idx.get()));

  OwnedNcObj obj0(NCVAR, "first_var", 1);
  OwnedNcObj obj2(NCVAR, "third_var", 3);

  // Setting index 2 on an empty index leaves NULL holes at indices 0 and 1.
  EXPECT_EQ(1, ncindexset(idx.get(), 2, &obj2.obj));
  EXPECT_EQ(3U, ncindexsize(idx.get()));
  EXPECT_EQ(1, ncindexcount(idx.get()));
  EXPECT_EQ(nullptr, ncindexith(idx.get(), 0));
  EXPECT_EQ(&obj2.obj, ncindexlookup(idx.get(), "third_var"));

  // Deleting index 0 removes the NULL slot from list; hashmapdeactivate returns
  // 0 because index 0 was never inserted into the hashmap.
  EXPECT_EQ(0, ncindexidel(idx.get(), 0));
  EXPECT_EQ(2U, ncindexsize(idx.get()));

  // Place obj0 at slot 0 (leaving NULL at slot 1).
  EXPECT_EQ(1, ncindexset(idx.get(), 0, &obj0.obj));
  EXPECT_EQ(&obj2.obj, ncindexith(idx.get(), 1));

  // Add a hole at index 2 via ncindexset at index 3 to verify NULL skipping in
  // ncindexrebuild() and pointer search across holes.
  OwnedNcObj obj3(NCDIM, "fourth_dim", 4);
  EXPECT_EQ(1, ncindexset(idx.get(), 3, &obj3.obj));
  EXPECT_EQ(3, ncindexfind(idx.get(), &obj3.obj));
  EXPECT_EQ(size_t{4}, ncindexsize(idx.get()));
  EXPECT_EQ(3, ncindexcount(idx.get()));

  // Rename obj0 before rebuilding to verify hashkeys and indices update.
  obj0.Rename("renamed_var");
  EXPECT_EQ(1, ncindexrebuild(idx.get()));
  EXPECT_EQ(3U, ncindexsize(idx.get()));
  EXPECT_EQ(3, ncindexcount(idx.get()));
  EXPECT_EQ(nullptr, ncindexlookup(idx.get(), "first_var"));
  EXPECT_EQ(&obj0.obj, ncindexlookup(idx.get(), "renamed_var"));
  EXPECT_EQ(&obj2.obj, ncindexlookup(idx.get(), "third_var"));
  EXPECT_EQ(&obj3.obj, ncindexlookup(idx.get(), "fourth_dim"));

  // Delete the tail entry (valid in hashmap) and verify removal.
  EXPECT_EQ(1, ncindexidel(idx.get(), 2));
  EXPECT_EQ(2U, ncindexsize(idx.get()));
  EXPECT_EQ(nullptr, ncindexlookup(idx.get(), "fourth_dim"));
}

TEST(NcindexTest, PrintHelpersCoverAllSortNamesAndNullInputs) {
  printindexlist(nullptr);
  printindexmap(nullptr);
  printindex(nullptr);

  ScopedNcIndex idx(ncindexnew(0));
  ASSERT_NE(nullptr, idx.get());

  OwnedNcObj s_nat(NCNAT, "s_nat", 0);
  OwnedNcObj s_var(NCVAR, "s_var", 1);
  OwnedNcObj s_dim(NCDIM, "s_dim", 2);
  OwnedNcObj s_att(NCATT, "s_att", 3);
  OwnedNcObj s_typ(NCTYP, "s_typ", 4);
  OwnedNcObj s_grp(NCGRP, "s_grp", 5);
  OwnedNcObj s_fld(NCFLD, "s_fld", 6);
  OwnedNcObj s_gap(NCVAR, "s_after_gap", 8);

  EXPECT_EQ(1, ncindexadd(idx.get(), &s_nat.obj));
  EXPECT_EQ(1, ncindexadd(idx.get(), &s_var.obj));
  EXPECT_EQ(1, ncindexadd(idx.get(), &s_dim.obj));
  EXPECT_EQ(1, ncindexadd(idx.get(), &s_att.obj));
  EXPECT_EQ(1, ncindexadd(idx.get(), &s_typ.obj));
  EXPECT_EQ(1, ncindexadd(idx.get(), &s_grp.obj));
  EXPECT_EQ(1, ncindexadd(idx.get(), &s_fld.obj));
  // Create a NULL entry at index 7 to cover the o == NULL branch.
  EXPECT_EQ(1, ncindexset(idx.get(), 8, &s_gap.obj));

  printindex(idx.get());
}

TEST(NcindexTest, VerifyCoversEmptyConsistentAndMismatchStates) {
  // 1. Empty index with dump disabled and enabled.
  {
    ScopedNcIndex empty(ncindexnew(0));
    ASSERT_NE(nullptr, empty.get());
    EXPECT_EQ(1, ncindexverify(empty.get(), 0));
    EXPECT_EQ(1, ncindexverify(empty.get(), 1));
  }

  // 2. Consistent non-empty index using long names (>= 8 chars) hashing into
  // table slots 0 and 1 so ncindexverify()'s m < active scan finds both.
  ScopedNcIndex idx(ncindexnew(0));
  ASSERT_NE(nullptr, idx.get());
  const size_t alloc = idx->map->alloc;
  ASSERT_GE(alloc, size_t{12});

  std::string key_slot0;
  std::string key_slot1;
  std::string replacement_key;
  for (int i = 0; i < 10000 && (key_slot0.empty() || key_slot1.empty() ||
                                replacement_key.empty());
       ++i) {
    std::string candidate = "verify_slot_key_" + std::to_string(i);
    const size_t slot = static_cast<size_t>(
        NC_hashmapkey(candidate.data(), candidate.size()) % alloc);
    if (slot == 0 && key_slot0.empty()) {
      key_slot0 = std::move(candidate);
    } else if (slot == 1 && key_slot1.empty()) {
      key_slot1 = std::move(candidate);
    } else if (slot >= 10 && replacement_key.empty()) {
      replacement_key = std::move(candidate);
    }
  }
  ASSERT_FALSE(key_slot0.empty());
  ASSERT_FALSE(key_slot1.empty());
  ASSERT_FALSE(replacement_key.empty());

  VerifyShimObj obj0(key_slot0, 1);
  VerifyShimObj obj1(key_slot1, 2);
  ASSERT_NE(nullptr, obj0.get());
  ASSERT_NE(nullptr, obj1.get());
  ASSERT_EQ(1, ncindexadd(idx.get(), obj0.get()));
  ASSERT_EQ(1, ncindexadd(idx.get(), obj1.get()));
  EXPECT_EQ(1, ncindexverify(idx.get(), 1));

  // 3. Corrupt index[0] by replacing list[0] without removing key_slot0 from
  // map: choosing replacement_key at table slot >= 10 deterministically covers
  // name mismatch, in-vector-not-in-map, and in-hash-not-in-vector.
  VerifyShimObj replacement(replacement_key, 3);
  ASSERT_NE(nullptr, replacement.get());
  ASSERT_EQ(1, ncindexset(idx.get(), 0, replacement.get()));
  EXPECT_EQ(0, ncindexverify(idx.get(), 0));

  // 4. Deleting slot 0 leaves the next entry's hashmap data pointing past end
  // of list, covering the bad-data (object == NULL) check.
  ScopedNcIndex stale_idx(ncindexnew(0));
  ASSERT_NE(nullptr, stale_idx.get());
  VerifyShimObj s0("long_stale_entry_zero", 10);
  VerifyShimObj s1("long_stale_entry_one", 11);
  ASSERT_NE(nullptr, s0.get());
  ASSERT_NE(nullptr, s1.get());
  ASSERT_EQ(1, ncindexadd(stale_idx.get(), s0.get()));
  ASSERT_EQ(1, ncindexadd(stale_idx.get(), s1.get()));
  ASSERT_EQ(1, ncindexidel(stale_idx.get(), 0));
  EXPECT_EQ(0, ncindexverify(stale_idx.get(), 0));

  // 5. Mutating name in-place triggers name mismatch after rebuild resets map.
  ASSERT_EQ(1, ncindexrebuild(stale_idx.get()));
  s1.Rename("long_renamed_entry_one");
  EXPECT_EQ(0, ncindexverify(stale_idx.get(), 0));
}

}  // namespace
}  // namespace netcdf
