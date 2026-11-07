// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/hproto.h"
#include "third_party/hdf4/hdf/src/vg.h"

extern "C" void vprint(void* k1);

namespace hdf4 {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::Ne;
using ::testing::StrEq;

constexpr int32_t kFakeVgid = (3 << 28) | 0x1234;
constexpr int32_t kFakeVsid = (4 << 28) | 0x1234;
constexpr int32_t kFakeFid = (1 << 28) | 0x1234;

std::string GetTempFilePath(std::string_view filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

TEST(VgpTest, VopenVcloseAndFileLifecycle) {
  const std::string path = GetTempFilePath("vgp_open_close.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  char open_path[512] = {0};
  std::strncpy(open_path, path.c_str(), sizeof(open_path) - 1);

  // 1. Vopen in CREATE mode
  const int32_t fid = Vopen(open_path, DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));

  // Create a Vgroup to write data into the file
  const int32_t vgid = Vattach(fid, -1, "w");
  ASSERT_THAT(vgid, Ne(FAIL));
  EXPECT_THAT(Vsetname(vgid, "Group1"), Eq(SUCCEED));
  EXPECT_THAT(Vsetclass(vgid, "Class1"), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(vgid), Eq(SUCCEED));

  // Vclose closes the file
  EXPECT_THAT(Vclose(fid), Eq(SUCCEED));

  // 2. Vopen in READ mode
  const int32_t rfid = Vopen(open_path, DFACC_READ, 0);
  ASSERT_THAT(rfid, Ne(FAIL));

  const int32_t rvg_ref = Vgetid(rfid, -1);
  EXPECT_THAT(rvg_ref, Gt(0));
  EXPECT_THAT(vexistvg(rfid, static_cast<uint16_t>(rvg_ref)), Eq(1));

  EXPECT_THAT(Vclose(rfid), Eq(SUCCEED));

  // 3. Open with Hopen + Vinitialize + Vfinish + Hclose
  const int32_t hfid = Hopen(open_path, DFACC_READ, 0);
  ASSERT_THAT(hfid, Ne(FAIL));
  EXPECT_THAT(Vinitialize(hfid), Eq(SUCCEED));
  EXPECT_THAT(Vfinish(hfid), Eq(SUCCEED));
  EXPECT_THAT(Hclose(hfid), Eq(SUCCEED));
}

TEST(VgpTest, VgroupCreationNamingAndInquire) {
  const std::string path = GetTempFilePath("vgp_props.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
  absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

  const int32_t vgid = Vattach(fid, -1, "w");
  ASSERT_THAT(vgid, Ne(FAIL));

  // Query tag and ref
  EXPECT_THAT(VQuerytag(vgid), Eq(DFTAG_VG));
  const int32_t vg_ref = VQueryref(vgid);
  EXPECT_THAT(vg_ref, Gt(0));

  // Initially, name and class are not set (namelen = 0)
  uint16_t namelen = 99, classnamelen = 99;
  EXPECT_THAT(Vgetnamelen(vgid, &namelen), Eq(SUCCEED));
  EXPECT_THAT(namelen, Eq(0));
  EXPECT_THAT(Vgetclassnamelen(vgid, &classnamelen), Eq(SUCCEED));
  EXPECT_THAT(classnamelen, Eq(0));

  // Set name and class
  EXPECT_THAT(Vsetname(vgid, "TestVgroup"), Eq(SUCCEED));
  EXPECT_THAT(Vsetclass(vgid, "TestGroupClass"), Eq(SUCCEED));

  // Query lengths
  EXPECT_THAT(Vgetnamelen(vgid, &namelen), Eq(SUCCEED));
  EXPECT_THAT(namelen, Eq(std::strlen("TestVgroup")));
  EXPECT_THAT(Vgetclassnamelen(vgid, &classnamelen), Eq(SUCCEED));
  EXPECT_THAT(classnamelen, Eq(std::strlen("TestGroupClass")));

  // Query name and class
  char name_buf[VGNAMELENMAX + 1] = {0};
  char class_buf[VGNAMELENMAX + 1] = {0};
  EXPECT_THAT(Vgetname(vgid, name_buf), Eq(SUCCEED));
  EXPECT_THAT(std::string_view(name_buf), StrEq("TestVgroup"));
  EXPECT_THAT(Vgetclass(vgid, class_buf), Eq(SUCCEED));
  EXPECT_THAT(std::string_view(class_buf), StrEq("TestGroupClass"));

  // Vinquire
  int32_t nentries = -1;
  char inq_name[VGNAMELENMAX + 1] = {0};
  EXPECT_THAT(Vinquire(vgid, &nentries, inq_name), Eq(SUCCEED));
  EXPECT_THAT(nentries, Eq(0));
  EXPECT_THAT(std::string_view(inq_name), StrEq("TestVgroup"));

  // Vinquire with nullptrs
  EXPECT_THAT(Vinquire(vgid, nullptr, nullptr), Eq(SUCCEED));
  EXPECT_THAT(Vinquire(vgid, &nentries, nullptr), Eq(SUCCEED));
  EXPECT_THAT(Vinquire(vgid, nullptr, inq_name), Eq(SUCCEED));

  // Truncate name and class when exceeding VGNAMELENMAX
  const std::string too_long_name(VGNAMELENMAX + 20, 'N');
  EXPECT_THAT(Vsetname(vgid, too_long_name.c_str()), Eq(SUCCEED));
  EXPECT_THAT(Vgetnamelen(vgid, &namelen), Eq(SUCCEED));
  EXPECT_THAT(namelen, Eq(too_long_name.size()));

  const std::string too_long_class(VGNAMELENMAX + 20, 'C');
  EXPECT_THAT(Vsetclass(vgid, too_long_class.c_str()), Eq(SUCCEED));
  EXPECT_THAT(Vgetclassnamelen(vgid, &classnamelen), Eq(SUCCEED));
  EXPECT_THAT(classnamelen, Eq(too_long_class.size()));

  EXPECT_THAT(Vdetach(vgid), Eq(SUCCEED));
}

TEST(VgpTest, ElementsMembershipAndNavigation) {
  const std::string path = GetTempFilePath("vgp_elements.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
  absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

  // Create Parent Vgroup
  const int32_t parent_vg = Vattach(fid, -1, "w");
  ASSERT_THAT(parent_vg, Ne(FAIL));
  EXPECT_THAT(Vsetname(parent_vg, "ParentVg"), Eq(SUCCEED));

  // Create Child Vgroup
  const int32_t child_vg = Vattach(fid, -1, "w");
  ASSERT_THAT(child_vg, Ne(FAIL));
  EXPECT_THAT(Vsetname(child_vg, "ChildVg"), Eq(SUCCEED));
  const int32_t child_vg_ref = VQueryref(child_vg);

  // Create Vdata with field "Temperature"
  const int32_t vs = VSattach(fid, -1, "w");
  ASSERT_THAT(vs, Ne(FAIL));
  EXPECT_THAT(VSsetname(vs, "SensorData"), Eq(SUCCEED));
  EXPECT_THAT(VSfdefine(vs, "Temperature", DFNT_FLOAT32, 1), Eq(SUCCEED));
  EXPECT_THAT(VSsetfields(vs, "Temperature"), Eq(SUCCEED));
  const float temp_val = 23.5f;
  EXPECT_THAT(VSwrite(vs, reinterpret_cast<const uint8_t*>(&temp_val), 1,
                      FULL_INTERLACE),
              Eq(1));
  const int32_t vs_ref = VSQueryref(vs);

  // Insert child_vg and vs into parent_vg using Vinsert
  EXPECT_THAT(Vinsert(parent_vg, child_vg), Ge(0));
  EXPECT_THAT(Vinsert(parent_vg, vs), Ge(0));

  // Add arbitrary tag/ref pairs using Vaddtagref
  EXPECT_THAT(Vaddtagref(parent_vg, DFTAG_RIG, 101), Gt(0));
  EXPECT_THAT(Vaddtagref(parent_vg, DFTAG_RIG, 102), Gt(0));

  // Detach child and vdata now that they are inserted
  EXPECT_THAT(Vdetach(child_vg), Eq(SUCCEED));
  EXPECT_THAT(VSdetach(vs), Eq(SUCCEED));

  // Check counts: total 4 elements (1 child vg, 1 vs, 2 RIGs)
  EXPECT_THAT(Vntagrefs(parent_vg), Eq(4));
  EXPECT_THAT(Vnrefs(parent_vg, DFTAG_VG), Eq(1));
  EXPECT_THAT(Vnrefs(parent_vg, DFTAG_VH), Eq(1));
  EXPECT_THAT(Vnrefs(parent_vg, DFTAG_RIG), Eq(2));
  EXPECT_THAT(Vnrefs(parent_vg, DFTAG_NULL), Eq(0));

  // Check membership with Vinqtagref
  EXPECT_THAT(Vinqtagref(parent_vg, DFTAG_VG, child_vg_ref), Eq(1));
  EXPECT_THAT(Vinqtagref(parent_vg, DFTAG_VH, vs_ref), Eq(1));
  EXPECT_THAT(Vinqtagref(parent_vg, DFTAG_RIG, 101), Eq(1));
  EXPECT_THAT(Vinqtagref(parent_vg, DFTAG_RIG, 999), Eq(0));

  // Predicates: Visvg and Visvs
  EXPECT_THAT(Visvg(parent_vg, child_vg_ref), Eq(1));
  EXPECT_THAT(Visvg(parent_vg, vs_ref), Eq(0));
  EXPECT_THAT(Visvg(parent_vg, 999), Eq(0));

  EXPECT_THAT(Visvs(parent_vg, vs_ref), Eq(1));
  EXPECT_THAT(Visvs(parent_vg, child_vg_ref), Eq(0));
  EXPECT_THAT(Visvs(parent_vg, 999), Eq(0));

  // Vflocate: locates vdata containing field "Temperature"
  char fld_temp[] = "Temperature";
  EXPECT_THAT(Vflocate(parent_vg, fld_temp), Eq(vs_ref));
  char fld_bad[] = "NonExistentField";
  EXPECT_THAT(Vflocate(parent_vg, fld_bad), Eq(FAIL));

  // Vgettagref: lookup by index
  int32_t tag = 0, ref = 0;
  EXPECT_THAT(Vgettagref(parent_vg, 0, &tag, &ref), Eq(SUCCEED));
  EXPECT_THAT(tag, Eq(DFTAG_VG));
  EXPECT_THAT(ref, Eq(child_vg_ref));

  EXPECT_THAT(Vgettagref(parent_vg, 1, &tag, &ref), Eq(SUCCEED));
  EXPECT_THAT(tag, Eq(DFTAG_VH));
  EXPECT_THAT(ref, Eq(vs_ref));

  // Out of bounds index
  EXPECT_THAT(Vgettagref(parent_vg, 99, &tag, &ref), Eq(FAIL));

  // Vgettagrefs: bulk retrieval
  int32_t tags[10] = {0};
  int32_t refs[10] = {0};
  EXPECT_THAT(Vgettagrefs(parent_vg, tags, refs, 4), Eq(4));
  EXPECT_THAT(tags[0], Eq(DFTAG_VG));
  EXPECT_THAT(refs[0], Eq(child_vg_ref));
  EXPECT_THAT(tags[1], Eq(DFTAG_VH));
  EXPECT_THAT(refs[1], Eq(vs_ref));
  EXPECT_THAT(tags[2], Eq(DFTAG_RIG));
  EXPECT_THAT(refs[2], Eq(101));
  EXPECT_THAT(tags[3], Eq(DFTAG_RIG));
  EXPECT_THAT(refs[3], Eq(102));

  // Requesting more tags than present clamps to nvelt (line 1746)
  int32_t big_tags[20] = {0};
  int32_t big_refs[20] = {0};
  EXPECT_THAT(Vgettagrefs(parent_vg, big_tags, big_refs, 20), Eq(4));

  // Vgetnext: iterate vgroup/vdata elements in parent_vg
  const int32_t next1 = Vgetnext(parent_vg, -1);
  EXPECT_THAT(next1, Eq(child_vg_ref));
  const int32_t next2 = Vgetnext(parent_vg, next1);
  EXPECT_THAT(next2, Eq(vs_ref));
  // Non-vgroup/vdata elements cause Vgetnext to return FAIL
  EXPECT_THAT(Vgetnext(parent_vg, next2), Eq(FAIL));

  // Vdeletetagref: delete DFTAG_RIG ref 101
  EXPECT_THAT(Vdeletetagref(parent_vg, DFTAG_RIG, 101), Eq(SUCCEED));
  EXPECT_THAT(Vinqtagref(parent_vg, DFTAG_RIG, 101), Eq(0));
  EXPECT_THAT(Vntagrefs(parent_vg), Eq(3));

  // Deleting non-existent tag/ref fails
  EXPECT_THAT(Vdeletetagref(parent_vg, DFTAG_RIG, 999), Eq(FAIL));

  const int32_t parent_vg_ref = VQueryref(parent_vg);
  EXPECT_THAT(Vdetach(parent_vg), Eq(SUCCEED));

  // Ventries: query entries via file id and ref
  EXPECT_THAT(Ventries(fid, parent_vg_ref), Eq(3));
  EXPECT_THAT(Ventries(fid, child_vg_ref), Eq(0));
  EXPECT_THAT(Ventries(fid, 9999), Eq(FAIL));
}

TEST(VgpTest, VdeleteAndFileReopen) {
  const std::string path = GetTempFilePath("vgp_delete.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  int32_t vg1_ref = -1, vg2_ref = -1;

  // 1. Create file with 2 Vgroups
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    const int32_t vg1 = Vattach(fid, -1, "w");
    ASSERT_THAT(vg1, Ne(FAIL));
    EXPECT_THAT(Vsetname(vg1, "VgToKeep"), Eq(SUCCEED));
    vg1_ref = VQueryref(vg1);
    EXPECT_THAT(Vdetach(vg1), Eq(SUCCEED));

    const int32_t vg2 = Vattach(fid, -1, "w");
    ASSERT_THAT(vg2, Ne(FAIL));
    EXPECT_THAT(Vsetname(vg2, "VgToDelete"), Eq(SUCCEED));
    vg2_ref = VQueryref(vg2);
    EXPECT_THAT(Vdetach(vg2), Eq(SUCCEED));

    // Delete vg2
    EXPECT_THAT(Vdelete(fid, vg2_ref), Eq(SUCCEED));

    // Deleting non-existent vgroup fails
    EXPECT_THAT(Vdelete(fid, 9999), Eq(FAIL));
  }

  // 2. Re-open in read mode: vg1 should exist, vg2 should be gone
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    EXPECT_THAT(vexistvg(fid, static_cast<uint16_t>(vg1_ref)), Eq(1));
    EXPECT_THAT(vexistvg(fid, static_cast<uint16_t>(vg2_ref)), Eq(FAIL));

    EXPECT_THAT(Vfind(fid, "VgToKeep"), Eq(vg1_ref));
    EXPECT_THAT(Vfind(fid, "VgToDelete"), Eq(0));
  }
}

TEST(VgpTest, InternalVsUserCreatedVgroups) {
  const std::string path = GetTempFilePath("vgp_internal.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  // Test Visinternal directly
  EXPECT_THAT(Visinternal(_HDF_VARIABLE), Eq(1));
  EXPECT_THAT(Visinternal(_HDF_DIMENSION), Eq(1));
  EXPECT_THAT(Visinternal(_HDF_UDIMENSION), Eq(1));
  EXPECT_THAT(Visinternal(_HDF_CDF), Eq(1));
  EXPECT_THAT(Visinternal(GR_NAME), Eq(1));
  EXPECT_THAT(Visinternal(RI_NAME), Eq(1));
  EXPECT_THAT(Visinternal("UserClass"), Eq(0));
  EXPECT_THAT(Visinternal(""), Eq(0));

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
  absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

  // User Vgroup 1
  const int32_t vg_user1 = Vattach(fid, -1, "w");
  ASSERT_THAT(vg_user1, Ne(FAIL));
  EXPECT_THAT(Vsetname(vg_user1, "UserGroup1"), Eq(SUCCEED));
  EXPECT_THAT(Vsetclass(vg_user1, "UserClass"), Eq(SUCCEED));
  EXPECT_THAT(Vgisinternal(vg_user1), Eq(0));
  const int32_t user1_ref = VQueryref(vg_user1);

  // User Vgroup 2 (sub-group of user1)
  const int32_t vg_user2 = Vattach(fid, -1, "w");
  ASSERT_THAT(vg_user2, Ne(FAIL));
  EXPECT_THAT(Vsetname(vg_user2, "UserGroup2"), Eq(SUCCEED));
  EXPECT_THAT(Vsetclass(vg_user2, "UserSubClass"), Eq(SUCCEED));
  EXPECT_THAT(Vgisinternal(vg_user2), Eq(0));
  const int32_t user2_ref = VQueryref(vg_user2);
  EXPECT_THAT(Vinsert(vg_user1, vg_user2), Ge(0));
  EXPECT_THAT(Vdetach(vg_user2), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(vg_user1), Eq(SUCCEED));

  // Internal Vgroup with class _HDF_VARIABLE
  const int32_t vg_internal1 = Vattach(fid, -1, "w");
  ASSERT_THAT(vg_internal1, Ne(FAIL));
  EXPECT_THAT(Vsetclass(vg_internal1, _HDF_VARIABLE), Eq(SUCCEED));
  EXPECT_THAT(Vgisinternal(vg_internal1), Eq(1));
  EXPECT_THAT(Vdetach(vg_internal1), Eq(SUCCEED));

  // Internal Vgroup with internal class _HDF_DIMENSION and name starting with
  // GR_NAME
  const int32_t vg_internal2 = Vattach(fid, -1, "w");
  ASSERT_THAT(vg_internal2, Ne(FAIL));
  EXPECT_THAT(Vsetclass(vg_internal2, _HDF_DIMENSION), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(vg_internal2, GR_NAME), Eq(SUCCEED));
  EXPECT_THAT(Vgisinternal(vg_internal2), Eq(1));
  EXPECT_THAT(Vdetach(vg_internal2), Eq(SUCCEED));

  // Test Vgetvgroups on file id (returns only user-created vgroups: user1 and
  // user2)
  EXPECT_THAT(Vgetvgroups(fid, 0, 0, nullptr), Eq(2));
  uint16_t vgs[10] = {0};
  EXPECT_THAT(Vgetvgroups(fid, 0, 10, vgs), Eq(2));
  EXPECT_THAT(vgs[0], Eq(user1_ref));
  EXPECT_THAT(vgs[1], Eq(user2_ref));

  // Vgetvgroups with start_vg = 1
  uint16_t sub_vgs[5] = {0};
  EXPECT_THAT(Vgetvgroups(fid, 1, 5, sub_vgs), Eq(1));
  EXPECT_THAT(sub_vgs[0], Eq(user2_ref));

  // Test Vgetvgroups on vgroup id (user1 contains user2)
  const int32_t r_user1 = Vattach(fid, user1_ref, "r");
  ASSERT_THAT(r_user1, Ne(FAIL));
  EXPECT_THAT(Vgetvgroups(r_user1, 0, 0, nullptr), Eq(1));
  EXPECT_THAT(Vgetvgroups(r_user1, 0, 5, sub_vgs), Eq(1));
  EXPECT_THAT(sub_vgs[0], Eq(user2_ref));
  // start_vg beyond user-created vgroups returns FAIL (line 3155)
  EXPECT_THAT(Vgetvgroups(r_user1, 100, 5, sub_vgs), Eq(FAIL));
  EXPECT_THAT(Vdetach(r_user1), Eq(SUCCEED));
}

TEST(VgpTest, MultipleAttachAndReattach) {
  const std::string path = GetTempFilePath("vgp_multi_attach.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
  absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

  const int32_t vg = Vattach(fid, -1, "w");
  ASSERT_THAT(vg, Ne(FAIL));
  EXPECT_THAT(Vsetname(vg, "MultiAttachVg"), Eq(SUCCEED));
  const int32_t vg_ref = VQueryref(vg);
  EXPECT_THAT(Vdetach(vg), Eq(SUCCEED));

  // Attach multiple times with "r"
  const int32_t r1 = Vattach(fid, vg_ref, "r");
  ASSERT_THAT(r1, Ne(FAIL));

  const int32_t r2 = Vattach(fid, vg_ref, "r");
  ASSERT_THAT(r2, Ne(FAIL));

  // Both attachments share access, detaching each succeeds
  EXPECT_THAT(Vdetach(r2), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(r1), Eq(SUCCEED));

  // Attach again with "w" to modify
  const int32_t w1 = Vattach(fid, vg_ref, "w");
  ASSERT_THAT(w1, Ne(FAIL));
  EXPECT_THAT(Vsetname(w1, "RenamedMultiAttachVg"), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(w1), Eq(SUCCEED));
}

TEST(VgpTest, VgroupReopenWithAttributesAndVdata) {
  const std::string path = GetTempFilePath("vgp_attrs_reopen.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  int32_t vg_ref = FAIL;
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    const int32_t vgid = Vattach(fid, -1, "w");
    ASSERT_THAT(vgid, Ne(FAIL));
    vg_ref = VQueryref(vgid);

    EXPECT_THAT(Vsetname(vgid, "AttrGroup"), Eq(SUCCEED));
    EXPECT_THAT(Vsetclass(vgid, "AttrClass"), Eq(SUCCEED));

    const int32_t int_val = 12345;
    EXPECT_THAT(Vsetattr(vgid, "IntAttr", DFNT_INT32, 1, &int_val),
                Eq(SUCCEED));
    const char str_val[] = "HDF4Attrs";
    EXPECT_THAT(Vsetattr(vgid, "StrAttr", DFNT_CHAR8, sizeof(str_val), str_val),
                Eq(SUCCEED));

    // Multiple attach with uppercase 'W'
    const int32_t vgid_dup = Vattach(fid, vg_ref, "W");
    ASSERT_THAT(vgid_dup, Ne(FAIL));
    EXPECT_THAT(Vdetach(vgid_dup), Eq(SUCCEED));
    EXPECT_THAT(Vdetach(vgid), Eq(SUCCEED));

    // Also write a Vdata to trigger vdata loading upon reopen
    const int32_t vsid = VSattach(fid, -1, "w");
    ASSERT_THAT(vsid, Ne(FAIL));
    EXPECT_THAT(VSfdefine(vsid, "FieldA", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vsid, "FieldA"), Eq(SUCCEED));
    EXPECT_THAT(VSwrite(vsid, reinterpret_cast<const uint8_t*>(&int_val), 1,
                        FULL_INTERLACE),
                Eq(1));
    EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));
  }

  // Reopen and verify unpacked attributes and vdata
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_RDWR, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    const int32_t vgid = Vattach(fid, vg_ref, "w");
    ASSERT_THAT(vgid, Ne(FAIL));
    EXPECT_THAT(Vnattrs(vgid), Eq(2));
    EXPECT_THAT(Vfindattr(vgid, "IntAttr"), Ge(0));
    EXPECT_THAT(Vfindattr(vgid, "StrAttr"), Ge(0));

    // Modify reopened vgroup (tests HDreuse_tagref when new_vg == 0)
    EXPECT_THAT(Vaddtagref(vgid, DFTAG_RIG, 301), Ge(0));
    EXPECT_THAT(Vdetach(vgid), Eq(SUCCEED));
  }
}

TEST(VgpTest, VgroupElementExpansionAndDeletion) {
  const std::string path = GetTempFilePath("vgp_expansion.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
  absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

  const int32_t vgid = Vattach(fid, -1, "w");
  ASSERT_THAT(vgid, Ne(FAIL));

  // Add 70 elements to exceed MAXNVELT (64), testing reallocation in
  // vinsertpair
  for (int32_t i = 1; i <= 70; ++i) {
    EXPECT_THAT(Vaddtagref(vgid, DFTAG_RIG, i), Eq(i));
  }
  EXPECT_THAT(Vntagrefs(vgid), Eq(70));

  // Delete the last element (hits non-shifting branch)
  EXPECT_THAT(Vdeletetagref(vgid, DFTAG_RIG, 70), Eq(SUCCEED));
  EXPECT_THAT(Vntagrefs(vgid), Eq(69));

  // Delete an element in the middle (hits shifting branch)
  EXPECT_THAT(Vdeletetagref(vgid, DFTAG_RIG, 35), Eq(SUCCEED));
  EXPECT_THAT(Vntagrefs(vgid), Eq(68));

  // Deleting non-existent element returns FAIL
  EXPECT_THAT(Vdeletetagref(vgid, DFTAG_RIG, 9999), Eq(FAIL));

  EXPECT_THAT(Vdetach(vgid), Eq(SUCCEED));
}

TEST(VgpTest, CrossFileAndDuplicateInsertErrors) {
  const std::string path1 = GetTempFilePath("vgp_cross1.hdf");
  const std::string path2 = GetTempFilePath("vgp_cross2.hdf");
  absl::Cleanup remove_temp = [&path1, &path2] {
    std::filesystem::remove(path1);
    std::filesystem::remove(path2);
  };

  const int32_t fid1 = Hopen(path1.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid1, Ne(FAIL));
  absl::Cleanup close_fid1 = [fid1] { EXPECT_THAT(Hclose(fid1), Eq(SUCCEED)); };
  ASSERT_THAT(Vstart(fid1), Eq(SUCCEED));
  absl::Cleanup end_v1 = [fid1] { EXPECT_THAT(Vend(fid1), Eq(SUCCEED)); };

  const int32_t fid2 = Hopen(path2.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid2, Ne(FAIL));
  absl::Cleanup close_fid2 = [fid2] { EXPECT_THAT(Hclose(fid2), Eq(SUCCEED)); };
  ASSERT_THAT(Vstart(fid2), Eq(SUCCEED));
  absl::Cleanup end_v2 = [fid2] { EXPECT_THAT(Vend(fid2), Eq(SUCCEED)); };

  const int32_t vg1 = Vattach(fid1, -1, "w");
  ASSERT_THAT(vg1, Ne(FAIL));
  absl::Cleanup detach_vg1 = [vg1] { EXPECT_THAT(Vdetach(vg1), Eq(SUCCEED)); };

  const int32_t child_vg1 = Vattach(fid1, -1, "w");
  ASSERT_THAT(child_vg1, Ne(FAIL));
  absl::Cleanup detach_child = [child_vg1] {
    EXPECT_THAT(Vdetach(child_vg1), Eq(SUCCEED));
  };

  const int32_t vs2 = VSattach(fid2, -1, "w");
  ASSERT_THAT(vs2, Ne(FAIL));
  absl::Cleanup detach_vs2 = [vs2] { EXPECT_THAT(VSdetach(vs2), Eq(SUCCEED)); };

  // Inserting element from different file returns FAIL (DFE_DIFFFILES)
  EXPECT_THAT(Vinsert(vg1, vs2), Eq(FAIL));

  // First insert succeeds, duplicate insert returns FAIL (DFE_DUPDD)
  EXPECT_THAT(Vinsert(vg1, child_vg1), Ge(0));
  EXPECT_THAT(Vinsert(vg1, child_vg1), Eq(FAIL));
}

TEST(VgpTest, VgisinternalNameCheckAndDefaults) {
  const std::string path = GetTempFilePath("vgp_defaults.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
  absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

  // New vgroup with no name and no class set
  const int32_t vgid = Vattach(fid, -1, "w");
  ASSERT_THAT(vgid, Ne(FAIL));

  char name_buf[64] = "initial";
  EXPECT_THAT(Vgetname(vgid, name_buf), Eq(SUCCEED));
  EXPECT_THAT(name_buf[0], Eq('\0'));

  char class_buf[64] = "initial";
  EXPECT_THAT(Vgetclass(vgid, class_buf), Eq(SUCCEED));
  EXPECT_THAT(class_buf[0], Eq('\0'));

  uint16_t len = 99;
  EXPECT_THAT(Vgetnamelen(vgid, &len), Eq(SUCCEED));
  EXPECT_THAT(len, Eq(0));

  EXPECT_THAT(Vgetclassnamelen(vgid, &len), Eq(SUCCEED));
  EXPECT_THAT(len, Eq(0));

  // Empty vgroup has no next elements
  EXPECT_THAT(Vgetnext(vgid, -1), Eq(FAIL));

  // Add non-VG/VS tag to test Vgetnext line 2381
  EXPECT_THAT(Vaddtagref(vgid, DFTAG_RIG, 501), Ge(0));
  EXPECT_THAT(Vgetnext(vgid, -1), Eq(FAIL));

  // Set name starting with GR_NAME without class to test Vgisinternal line 2947
  EXPECT_THAT(Vsetname(vgid, GR_NAME), Eq(SUCCEED));
  EXPECT_THAT(Vgisinternal(vgid), Eq(1));

  EXPECT_THAT(Vdetach(vgid), Eq(SUCCEED));
}

TEST(VgpTest, VgetidAndVgetnextNavigation) {
  const std::string path = GetTempFilePath("vgp_nav.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  // Test empty file with no vgroups
  {
    const int32_t empty_fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(empty_fid, Ne(FAIL));
    ASSERT_THAT(Vstart(empty_fid), Eq(SUCCEED));
    EXPECT_THAT(Vgetid(empty_fid, -1), Eq(FAIL));
    EXPECT_THAT(Vend(empty_fid), Eq(SUCCEED));
    EXPECT_THAT(Hclose(empty_fid), Eq(SUCCEED));
  }

  // Populate file with 3 vgroups and hierarchical children
  int32_t ref1 = FAIL, ref2 = FAIL, ref3 = FAIL;
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_RDWR, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    const int32_t vg1 = Vattach(fid, -1, "w");
    ASSERT_THAT(vg1, Ne(FAIL));
    ref1 = VQueryref(vg1);

    const int32_t vg2 = Vattach(fid, -1, "w");
    ASSERT_THAT(vg2, Ne(FAIL));
    ref2 = VQueryref(vg2);

    const int32_t vg3 = Vattach(fid, -1, "w");
    ASSERT_THAT(vg3, Ne(FAIL));
    ref3 = VQueryref(vg3);

    // vg1 contains vg2 and vg3
    EXPECT_THAT(Vinsert(vg1, vg2), Ge(0));
    EXPECT_THAT(Vinsert(vg1, vg3), Ge(0));

    // Vgetnext on vg1: first is vg2, next is vg3, next after vg3 is FAIL (last)
    EXPECT_THAT(Vgetnext(vg1, -1), Eq(ref2));
    EXPECT_THAT(Vgetnext(vg1, ref2), Eq(ref3));
    EXPECT_THAT(Vgetnext(vg1, ref3), Eq(FAIL));
    EXPECT_THAT(Vgetnext(vg1, 9999), Eq(FAIL));

    EXPECT_THAT(Vdetach(vg3), Eq(SUCCEED));
    EXPECT_THAT(Vdetach(vg2), Eq(SUCCEED));
    EXPECT_THAT(Vdetach(vg1), Eq(SUCCEED));

    // Vgetid tree navigation across all vgroups
    const int32_t first = Vgetid(fid, -1);
    EXPECT_THAT(first, Ne(FAIL));
    const int32_t second = Vgetid(fid, first);
    EXPECT_THAT(second, Ne(FAIL));
    const int32_t third = Vgetid(fid, second);
    EXPECT_THAT(third, Ne(FAIL));
    EXPECT_THAT(Vgetid(fid, third), Eq(FAIL));
    EXPECT_THAT(Vgetid(fid, 99999), Eq(FAIL));
  }
}

TEST(VgpTest, AccessModesAndShutdownLifecycle) {
  const std::string path = GetTempFilePath("vgp_modes.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  // Create file
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    const int32_t vgid = Vattach(fid, -1, "w");
    ASSERT_THAT(vgid, Ne(FAIL));
    EXPECT_THAT(Vdetach(vgid), Eq(SUCCEED));
    EXPECT_THAT(Vend(fid), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Open read-only and test permission violations
  {
    const int32_t rfid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(rfid, Ne(FAIL));
    absl::Cleanup close_r = [rfid] { EXPECT_THAT(Hclose(rfid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(rfid), Eq(SUCCEED));
    absl::Cleanup end_vr = [rfid] { EXPECT_THAT(Vend(rfid), Eq(SUCCEED)); };

    // Cannot create new vgroup in read-only file
    EXPECT_THAT(Vattach(rfid, -1, "w"), Eq(FAIL));

    // Invalid access mode
    EXPECT_THAT(Vattach(rfid, 1, "invalid_mode"), Eq(FAIL));

    // Cannot create new vgroup with read access
    EXPECT_THAT(Vattach(rfid, -1, "r"), Eq(FAIL));

    // Non-existent vgroup
    EXPECT_THAT(Vattach(rfid, 9999, "r"), Eq(FAIL));

    // Vdelete on read-only file
    EXPECT_THAT(Vdelete(rfid, 1), Eq(FAIL));

    // Vdelete with invalid vgid
    EXPECT_THAT(Vdelete(rfid, -1), Eq(FAIL));
  }

  // Test multiple Vinitialize and Vfinish, followed by VPshutdown with open
  // vfile
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_RDWR, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_f = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    EXPECT_THAT(Vinitialize(fid), Eq(SUCCEED));
    // Calling Vinitialize a second time increments access count and succeeds
    EXPECT_THAT(Vinitialize(fid), Eq(SUCCEED));

    // Vfinish decrements access count
    EXPECT_THAT(Vfinish(fid), Eq(SUCCEED));
    // Second Vfinish frees the vfile record
    EXPECT_THAT(Vfinish(fid), Eq(SUCCEED));
  }

  // Test VPshutdown
  EXPECT_THAT(VPshutdown(), Eq(SUCCEED));

  // Reopening after VPshutdown triggers Vgbuf allocation in VPgetinfo (lines
  // 1012-1018)
  {
    const int32_t fid2 = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid2, Ne(FAIL));
    EXPECT_THAT(Vstart(fid2), Eq(SUCCEED));
    EXPECT_THAT(Vend(fid2), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid2), Eq(SUCCEED));
  }
}

TEST(VgpTest, VprintCoverage) {
  int32_t dummy_node[4] = {1234, 5678, 0, 0};
  vprint(dummy_node);
}

TEST(VgpTest, OldStyleAttributeCleanup) {
  const std::string path = GetTempFilePath("vgp_old_attrs.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  int32_t vg1_ref = FAIL;
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    const int32_t vg1 = Vattach(fid, -1, "w");
    ASSERT_THAT(vg1, Ne(FAIL));
    vg1_ref = VQueryref(vg1);

    const int32_t vs = VSattach(fid, -1, "w");
    ASSERT_THAT(vs, Ne(FAIL));
    EXPECT_THAT(VSsetclass(vs, _HDF_ATTRIBUTE), Eq(SUCCEED));
    EXPECT_THAT(VSsetname(vs, "OldAttr"), Eq(SUCCEED));
    EXPECT_THAT(Vinsert(vg1, vs), Ge(0));
    EXPECT_THAT(VSdetach(vs), Eq(SUCCEED));

    // Vnoldattrs allocates vg1->old_alist
    EXPECT_THAT(Vnoldattrs(vg1), Eq(1));

    // Vdetach frees vg1->old_alist (hits lines 1292-1295)
    EXPECT_THAT(Vdetach(vg1), Eq(SUCCEED));
  }

  // Reopen, populate old_alist on an attached vgroup, and let Vend/Vfinish free
  // it
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));

    const int32_t vg1 = Vattach(fid, vg1_ref, "r");
    ASSERT_THAT(vg1, Ne(FAIL));
    EXPECT_THAT(Vnoldattrs(vg1), Eq(1));
    EXPECT_THAT(Vdetach(vg1), Eq(SUCCEED));

    EXPECT_THAT(Vend(fid), Eq(SUCCEED));
  }
}

TEST(VgpTest, ErrorBranchesAndInvalidArguments) {
  const std::string path = GetTempFilePath("vgp_errors.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
  absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

  const int32_t vgid = Vattach(fid, -1, "w");
  ASSERT_THAT(vgid, Ne(FAIL));
  absl::Cleanup detach_vg = [vgid] { EXPECT_THAT(Vdetach(vgid), Eq(SUCCEED)); };

  // Vattach errors
  EXPECT_THAT(Vattach(FAIL, -1, "w"), Eq(FAIL));
  EXPECT_THAT(Vattach(kFakeFid, -1, "w"), Eq(FAIL));
  EXPECT_THAT(Vattach(fid, -999, "w"), Eq(FAIL));
  EXPECT_THAT(Vattach(fid, 9999, "r"), Eq(FAIL));  // Non-existent ref

  // Vdetach errors
  EXPECT_THAT(Vdetach(FAIL), Eq(FAIL));
  EXPECT_THAT(Vdetach(kFakeVgid), Eq(FAIL));

  // Vinsert errors
  EXPECT_THAT(Vinsert(FAIL, vgid), Eq(FAIL));
  EXPECT_THAT(Vinsert(kFakeVgid, vgid), Eq(FAIL));
  EXPECT_THAT(Vinsert(vgid, FAIL), Eq(FAIL));
  EXPECT_THAT(Vinsert(vgid, kFakeVgid), Eq(FAIL));
  EXPECT_THAT(Vinsert(vgid, kFakeVsid), Eq(FAIL));

  // Vflocate errors
  char fld_x[] = "X";
  EXPECT_THAT(Vflocate(FAIL, fld_x), Eq(FAIL));
  EXPECT_THAT(Vflocate(kFakeVgid, fld_x), Eq(FAIL));
  EXPECT_THAT(Vflocate(vgid, nullptr), Eq(FAIL));

  // Vinqtagref errors
  EXPECT_THAT(Vinqtagref(FAIL, DFTAG_VG, 1), Eq(0));
  EXPECT_THAT(Vinqtagref(kFakeVgid, DFTAG_VG, 1), Eq(0));

  // Vdeletetagref errors
  EXPECT_THAT(Vdeletetagref(FAIL, DFTAG_VG, 1), Eq(FAIL));
  EXPECT_THAT(Vdeletetagref(kFakeVgid, DFTAG_VG, 1), Eq(FAIL));

  // Vntagrefs errors
  EXPECT_THAT(Vntagrefs(FAIL), Eq(FAIL));
  EXPECT_THAT(Vntagrefs(kFakeVgid), Eq(FAIL));

  // Vnrefs errors
  EXPECT_THAT(Vnrefs(FAIL, DFTAG_VG), Eq(FAIL));
  EXPECT_THAT(Vnrefs(kFakeVgid, DFTAG_VG), Eq(FAIL));

  // Vgettagrefs errors
  int32_t tags[5], refs[5];
  EXPECT_THAT(Vgettagrefs(FAIL, tags, refs, 5), Eq(FAIL));
  EXPECT_THAT(Vgettagrefs(kFakeVgid, tags, refs, 5), Eq(FAIL));

  // Vgettagref errors
  int32_t tag = 0, ref = 0;
  EXPECT_THAT(Vgettagref(FAIL, 0, &tag, &ref), Eq(FAIL));
  EXPECT_THAT(Vgettagref(kFakeVgid, 0, &tag, &ref), Eq(FAIL));
  EXPECT_THAT(Vgettagref(vgid, -1, &tag, &ref), Eq(FAIL));

  // VQuerytag and VQueryref errors
  EXPECT_THAT(VQuerytag(FAIL), Eq(FAIL));
  EXPECT_THAT(VQuerytag(kFakeVgid), Eq(FAIL));
  EXPECT_THAT(VQueryref(FAIL), Eq(FAIL));
  EXPECT_THAT(VQueryref(kFakeVgid), Eq(FAIL));

  // Vaddtagref errors
  EXPECT_THAT(Vaddtagref(FAIL, DFTAG_VG, 1), Eq(FAIL));
  EXPECT_THAT(Vaddtagref(kFakeVgid, DFTAG_VG, 1), Eq(FAIL));

  // Vsetname and Vsetclass errors
  EXPECT_THAT(Vsetname(FAIL, "Name"), Eq(FAIL));
  EXPECT_THAT(Vsetname(kFakeVgid, "Name"), Eq(FAIL));
  EXPECT_THAT(Vsetname(vgid, nullptr), Eq(FAIL));

  EXPECT_THAT(Vsetclass(FAIL, "Class"), Eq(FAIL));
  EXPECT_THAT(Vsetclass(kFakeVgid, "Class"), Eq(FAIL));
  EXPECT_THAT(Vsetclass(vgid, nullptr), Eq(FAIL));

  // Vgetnamelen and Vgetclassnamelen errors
  uint16_t len = 0;
  EXPECT_THAT(Vgetnamelen(FAIL, &len), Eq(FAIL));
  EXPECT_THAT(Vgetnamelen(kFakeVgid, &len), Eq(FAIL));

  EXPECT_THAT(Vgetclassnamelen(FAIL, &len), Eq(FAIL));
  EXPECT_THAT(Vgetclassnamelen(kFakeVgid, &len), Eq(FAIL));

  // Vgetname and Vgetclass errors
  char buf[64];
  EXPECT_THAT(Vgetname(FAIL, buf), Eq(FAIL));
  EXPECT_THAT(Vgetname(kFakeVgid, buf), Eq(FAIL));
  EXPECT_THAT(Vgetname(vgid, nullptr), Eq(FAIL));

  EXPECT_THAT(Vgetclass(FAIL, buf), Eq(FAIL));
  EXPECT_THAT(Vgetclass(kFakeVgid, buf), Eq(FAIL));
  EXPECT_THAT(Vgetclass(vgid, nullptr), Eq(FAIL));

  // Vinquire errors
  int32_t nentries = 0;
  EXPECT_THAT(Vinquire(FAIL, &nentries, buf), Eq(FAIL));
  EXPECT_THAT(Vinquire(kFakeVgid, &nentries, buf), Eq(FAIL));

  // Visvg and Visvs errors
  EXPECT_THAT(Visvg(FAIL, 1), Eq(0));
  EXPECT_THAT(Visvg(kFakeVgid, 1), Eq(0));
  EXPECT_THAT(Visvs(FAIL, 1), Eq(0));
  EXPECT_THAT(Visvs(kFakeVgid, 1), Eq(0));

  // Vgetid errors
  EXPECT_THAT(Vgetid(FAIL, -1), Eq(FAIL));
  EXPECT_THAT(Vgetid(kFakeFid, -1), Eq(FAIL));
  EXPECT_THAT(Vgetid(fid, -2), Eq(FAIL));

  // Vgetnext errors
  EXPECT_THAT(Vgetnext(FAIL, -1), Eq(FAIL));
  EXPECT_THAT(Vgetnext(kFakeVgid, -1), Eq(FAIL));
  EXPECT_THAT(Vgetnext(vgid, -2), Eq(FAIL));

  // Ventries errors with non-positive vgid
  EXPECT_THAT(Ventries(fid, 0), Eq(FAIL));
  EXPECT_THAT(Ventries(fid, -1), Eq(FAIL));

  // Vgisinternal errors
  EXPECT_THAT(Vgisinternal(FAIL), Eq(FAIL));
  EXPECT_THAT(Vgisinternal(kFakeVgid), Eq(FAIL));

  // Vgetvgroups errors
  uint16_t vgs[5];
  EXPECT_THAT(Vgetvgroups(FAIL, 0, 5, vgs), Eq(FAIL));
  EXPECT_THAT(Vgetvgroups(kFakeFid, 0, 5, vgs), Eq(FAIL));
  EXPECT_THAT(Vgetvgroups(fid, 0, 0, vgs),
              Eq(FAIL));  // count 0 with non-null refarray
  EXPECT_THAT(Vgetvgroups(fid, 100, 5, vgs),
              Eq(FAIL));  // start beyond total count

  // Vdelete errors
  EXPECT_THAT(Vdelete(FAIL, 1), Eq(FAIL));
  EXPECT_THAT(Vdelete(kFakeFid, 1), Eq(FAIL));

  // Vopen / Vclose errors
  char bad_path[] = "/invalid/nonexistent/dir/file.hdf";
  EXPECT_THAT(Vopen(bad_path, DFACC_READ, 0), Eq(FAIL));
  EXPECT_THAT(Vclose(FAIL), Eq(FAIL));
  EXPECT_THAT(Vclose(kFakeFid), Eq(FAIL));

  // Vinitialize / Vfinish errors
  EXPECT_THAT(Vinitialize(FAIL), Eq(FAIL));
  EXPECT_THAT(Vfinish(FAIL), Eq(FAIL));
}

}  // namespace
}  // namespace hdf4
