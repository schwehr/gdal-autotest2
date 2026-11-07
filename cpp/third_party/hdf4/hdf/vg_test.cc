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

#include "third_party/hdf4/hdf/src/vg.h"

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

namespace hdf4 {
namespace {

using ::testing::Eq;
using ::testing::Gt;
using ::testing::Ne;
using ::testing::StrEq;

constexpr int32_t kFakeVsid = (4 << 28) | 0x1234;
constexpr int32_t kFakeVgid = (3 << 28) | 0x1234;
constexpr int32_t kFakeFid = (1 << 28) | 0x1234;

std::string GetTempFilePath(std::string_view filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

TEST(VgTest, VdataBasicPropertiesAndInquire) {
  const std::string path = GetTempFilePath("vg_vdata_props.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  // 1. Create file and write a vdata
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    const int32_t vsid = VSattach(fid, -1, "w");
    ASSERT_THAT(vsid, Ne(FAIL));

    // Test initial block info and setting blocksize / numblocks
    int32_t blk_size = 0, num_blks = 0;
    EXPECT_THAT(VSgetblockinfo(vsid, &blk_size, &num_blks), Eq(SUCCEED));
    EXPECT_THAT(VSsetblocksize(vsid, 1024), Eq(SUCCEED));
    EXPECT_THAT(VSsetnumblocks(vsid, 4), Eq(SUCCEED));
    EXPECT_THAT(VSgetblockinfo(vsid, &blk_size, &num_blks), Eq(SUCCEED));
    EXPECT_THAT(blk_size, Eq(1024));
    EXPECT_THAT(num_blks, Eq(4));

    // Selective nullptrs for VSgetblockinfo
    EXPECT_THAT(VSgetblockinfo(vsid, nullptr, nullptr), Eq(SUCCEED));
    EXPECT_THAT(VSgetblockinfo(vsid, &blk_size, nullptr), Eq(SUCCEED));
    EXPECT_THAT(VSgetblockinfo(vsid, nullptr, &num_blks), Eq(SUCCEED));

    // Set name and class
    EXPECT_THAT(VSsetname(vsid, "TestVdata"), Eq(SUCCEED));
    EXPECT_THAT(VSsetclass(vsid, "CustomClass"), Eq(SUCCEED));

    // Query name and class
    char name_buf[VSNAMELENMAX + 1] = {0};
    char class_buf[VSNAMELENMAX + 1] = {0};
    EXPECT_THAT(VSgetname(vsid, name_buf), Eq(SUCCEED));
    EXPECT_THAT(std::string_view(name_buf), StrEq("TestVdata"));
    EXPECT_THAT(VSgetclass(vsid, class_buf), Eq(SUCCEED));
    EXPECT_THAT(std::string_view(class_buf), StrEq("CustomClass"));

    // Interlace testing
    EXPECT_THAT(VSgetinterlace(vsid), Eq(FULL_INTERLACE));
    EXPECT_THAT(VSsetinterlace(vsid, NO_INTERLACE), Eq(SUCCEED));
    EXPECT_THAT(VSgetinterlace(vsid), Eq(NO_INTERLACE));
    EXPECT_THAT(VSsetinterlace(vsid, FULL_INTERLACE), Eq(SUCCEED));
    EXPECT_THAT(VSgetinterlace(vsid), Eq(FULL_INTERLACE));

    // Define fields: Pos (FLOAT32 x 3), Temp (FLOAT32 x 1), ID (INT32 x 1)
    EXPECT_THAT(VSfdefine(vsid, "Pos", DFNT_FLOAT32, 3), Eq(SUCCEED));
    EXPECT_THAT(VSfdefine(vsid, "Temp", DFNT_FLOAT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSfdefine(vsid, "ID", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vsid, "Pos,Temp,ID"), Eq(SUCCEED));

    // VSgetfields
    char flds_buf[128] = {0};
    EXPECT_THAT(VSgetfields(vsid, flds_buf), Eq(3));
    EXPECT_THAT(std::string_view(flds_buf), StrEq("Pos,Temp,ID"));

    // VSfexist
    char fld_pos[] = "Pos";
    char fld_pos_temp[] = "Pos,Temp";
    char fld_id_pos_temp[] = "ID,Pos,Temp";
    char fld_nonexistent[] = "NonExistent";
    char fld_pos_nonexistent[] = "Pos,NonExistent";
    EXPECT_THAT(VSfexist(vsid, fld_pos), Eq(1));
    EXPECT_THAT(VSfexist(vsid, fld_pos_temp), Eq(1));
    EXPECT_THAT(VSfexist(vsid, fld_id_pos_temp), Eq(1));
    EXPECT_THAT(VSfexist(vsid, fld_nonexistent), Eq(FAIL));
    EXPECT_THAT(VSfexist(vsid, fld_pos_nonexistent), Eq(FAIL));

    // VSsizeof
    // Pos = 3 * 4 = 12 bytes; Temp = 4 bytes; ID = 4 bytes. Total = 20 bytes.
    char fld_temp[] = "Temp";
    char fld_id[] = "ID";
    char fld_all[] = "Pos,Temp,ID";
    char fld_empty[] = "";
    EXPECT_THAT(VSsizeof(vsid, fld_pos), Eq(12));
    EXPECT_THAT(VSsizeof(vsid, fld_temp), Eq(4));
    EXPECT_THAT(VSsizeof(vsid, fld_id), Eq(4));
    EXPECT_THAT(VSsizeof(vsid, fld_pos_temp), Eq(16));
    EXPECT_THAT(VSsizeof(vsid, fld_all), Eq(20));
    EXPECT_THAT(VSsizeof(vsid, nullptr), Eq(20));
    EXPECT_THAT(VSsizeof(vsid, fld_empty), Eq(FAIL));
    EXPECT_THAT(VSsizeof(vsid, fld_nonexistent), Eq(FAIL));

    // VSelts before write
    EXPECT_THAT(VSelts(vsid), Eq(0));

    // VSdump
    VSdump(vsid);

    // VSinquire before write
    int32_t nelt = -1, interlace = -1, eltsize = -1;
    char inq_fields[128] = {0};
    char inq_name[VSNAMELENMAX + 1] = {0};
    EXPECT_THAT(
        VSinquire(vsid, &nelt, &interlace, inq_fields, &eltsize, inq_name),
        Eq(SUCCEED));
    EXPECT_THAT(nelt, Eq(0));
    EXPECT_THAT(interlace, Eq(FULL_INTERLACE));
    EXPECT_THAT(eltsize, Eq(20));
    EXPECT_THAT(std::string_view(inq_fields), StrEq("Pos,Temp,ID"));
    EXPECT_THAT(std::string_view(inq_name), StrEq("TestVdata"));

    // VSinquire with selective nullptrs
    EXPECT_THAT(VSinquire(vsid, nullptr, nullptr, nullptr, nullptr, nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(VSinquire(vsid, &nelt, nullptr, nullptr, nullptr, nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(VSinquire(vsid, nullptr, &interlace, nullptr, nullptr, nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(VSinquire(vsid, nullptr, nullptr, inq_fields, nullptr, nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(VSinquire(vsid, nullptr, nullptr, nullptr, &eltsize, nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(VSinquire(vsid, nullptr, nullptr, nullptr, nullptr, inq_name),
                Eq(SUCCEED));

    // Write 3 records
    struct Record {
      float pos[3];
      float temp;
      int32_t id;
    } records[3] = {
        {{1.0f, 2.0f, 3.0f}, 98.6f, 101},
        {{4.0f, 5.0f, 6.0f}, 99.1f, 102},
        {{7.0f, 8.0f, 9.0f}, 97.4f, 103},
    };
    EXPECT_THAT(VSwrite(vsid, reinterpret_cast<const uint8_t*>(records), 3,
                        FULL_INTERLACE),
                Eq(3));

    // VSelts after write
    EXPECT_THAT(VSelts(vsid), Eq(3));

    // Setting interlace after write should fail
    EXPECT_THAT(VSsetinterlace(vsid, NO_INTERLACE), Eq(FAIL));

    EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));
  }

  // 2. Re-open in read mode and verify
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    const int32_t vs_ref = VSfind(fid, "TestVdata");
    ASSERT_THAT(vs_ref, Gt(0));

    const int32_t vsid = VSattach(fid, vs_ref, "r");
    ASSERT_THAT(vsid, Ne(FAIL));
    absl::Cleanup detach_vs = [vsid] {
      EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));
    };

    EXPECT_THAT(VSelts(vsid), Eq(3));
    EXPECT_THAT(VSgetinterlace(vsid), Eq(FULL_INTERLACE));

    // Setting interlace fails on read-only vdata (and after write)
    EXPECT_THAT(VSsetinterlace(vsid, NO_INTERLACE), Eq(FAIL));
    // Setting name and class in memory succeeds
    EXPECT_THAT(VSsetname(vsid, "NewName"), Eq(SUCCEED));
    EXPECT_THAT(VSsetclass(vsid, "NewClass"), Eq(SUCCEED));
  }
}

TEST(VgTest, VdataAndVgroupSearchAndLone) {
  const std::string path = GetTempFilePath("vg_search_lone.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  // Create file with hierarchy:
  // Lone Vgroup: "TopVg1" (class: "TopClass")
  // Lone Vgroup: "TopVg2" (class: "ContainerClass")
  // Child Vgroup: "ChildVg" (attached to TopVg2)
  // Lone Vdata: "LoneVs1" (class: "UserVsClass")
  // Child Vdata: "ChildVs" (attached to TopVg2, class: "ChildVsClass")
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    // Create TopVg1
    const int32_t vg1 = Vattach(fid, -1, "w");
    ASSERT_THAT(vg1, Ne(FAIL));
    EXPECT_THAT(Vsetname(vg1, "TopVg1"), Eq(SUCCEED));
    EXPECT_THAT(Vsetclass(vg1, "TopClass"), Eq(SUCCEED));
    const int32_t vg1_ref = VQueryref(vg1);
    EXPECT_THAT(Vdetach(vg1), Eq(SUCCEED));

    // Create ChildVg
    const int32_t vg_child = Vattach(fid, -1, "w");
    ASSERT_THAT(vg_child, Ne(FAIL));
    EXPECT_THAT(Vsetname(vg_child, "ChildVg"), Eq(SUCCEED));
    EXPECT_THAT(Vsetclass(vg_child, "ChildClass"), Eq(SUCCEED));
    const int32_t vg_child_ref = VQueryref(vg_child);
    EXPECT_THAT(Vdetach(vg_child), Eq(SUCCEED));

    // Create LoneVs1
    const int32_t vs_lone = VSattach(fid, -1, "w");
    ASSERT_THAT(vs_lone, Ne(FAIL));
    EXPECT_THAT(VSsetname(vs_lone, "LoneVs1"), Eq(SUCCEED));
    EXPECT_THAT(VSsetclass(vs_lone, "UserVsClass"), Eq(SUCCEED));
    EXPECT_THAT(VSfdefine(vs_lone, "F1", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs_lone, "F1"), Eq(SUCCEED));
    const int32_t d1 = 10;
    EXPECT_THAT(VSwrite(vs_lone, reinterpret_cast<const uint8_t*>(&d1), 1,
                        FULL_INTERLACE),
                Eq(1));
    const int32_t vs_lone_ref = VSQueryref(vs_lone);
    EXPECT_THAT(VSdetach(vs_lone), Eq(SUCCEED));

    // Create ChildVs
    const int32_t vs_child = VSattach(fid, -1, "w");
    ASSERT_THAT(vs_child, Ne(FAIL));
    EXPECT_THAT(VSsetname(vs_child, "ChildVs"), Eq(SUCCEED));
    EXPECT_THAT(VSsetclass(vs_child, "ChildVsClass"), Eq(SUCCEED));
    EXPECT_THAT(VSfdefine(vs_child, "F2", DFNT_FLOAT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs_child, "F2"), Eq(SUCCEED));
    const float d2 = 3.14f;
    EXPECT_THAT(VSwrite(vs_child, reinterpret_cast<const uint8_t*>(&d2), 1,
                        FULL_INTERLACE),
                Eq(1));
    const int32_t vs_child_ref = VSQueryref(vs_child);
    EXPECT_THAT(VSdetach(vs_child), Eq(SUCCEED));

    // Create TopVg2 and attach ChildVg and ChildVs
    const int32_t vg2 = Vattach(fid, -1, "w");
    ASSERT_THAT(vg2, Ne(FAIL));
    EXPECT_THAT(Vsetname(vg2, "TopVg2"), Eq(SUCCEED));
    EXPECT_THAT(Vsetclass(vg2, "ContainerClass"), Eq(SUCCEED));
    EXPECT_THAT(Vaddtagref(vg2, DFTAG_VG, vg_child_ref), Gt(0));
    EXPECT_THAT(Vaddtagref(vg2, DFTAG_VH, vs_child_ref), Gt(0));
    const int32_t vg2_ref = VQueryref(vg2);
    EXPECT_THAT(Vdetach(vg2), Eq(SUCCEED));

    // Test Vfind and VSfind
    EXPECT_THAT(Vfind(fid, "TopVg1"), Eq(vg1_ref));
    EXPECT_THAT(Vfind(fid, "TopVg2"), Eq(vg2_ref));
    EXPECT_THAT(Vfind(fid, "ChildVg"), Eq(vg_child_ref));
    EXPECT_THAT(Vfind(fid, "NonExistentVg"), Eq(0));

    EXPECT_THAT(VSfind(fid, "LoneVs1"), Eq(vs_lone_ref));
    EXPECT_THAT(VSfind(fid, "ChildVs"), Eq(vs_child_ref));
    EXPECT_THAT(VSfind(fid, "NonExistentVs"), Eq(0));

    // Test Vfindclass and VSfindclass
    EXPECT_THAT(Vfindclass(fid, "TopClass"), Eq(vg1_ref));
    EXPECT_THAT(Vfindclass(fid, "ContainerClass"), Eq(vg2_ref));
    EXPECT_THAT(Vfindclass(fid, "ChildClass"), Eq(vg_child_ref));
    EXPECT_THAT(Vfindclass(fid, "NonExistentClass"), Eq(0));

    EXPECT_THAT(VSfindclass(fid, "UserVsClass"), Eq(vs_lone_ref));
    EXPECT_THAT(VSfindclass(fid, "ChildVsClass"), Eq(vs_child_ref));
    EXPECT_THAT(VSfindclass(fid, "NonExistentClass"), Eq(0));

    // Test Vlone (TopVg1 and TopVg2 should be lone; ChildVg is not lone)
    EXPECT_THAT(Vlone(fid, nullptr, 0), Eq(2));
    EXPECT_THAT(Vlone(fid, nullptr, -1), Eq(2));
    int32_t lone_vgs[10] = {0};
    const int32_t n_lone_vgs = Vlone(fid, lone_vgs, 10);
    EXPECT_THAT(n_lone_vgs, Eq(2));
    EXPECT_TRUE((lone_vgs[0] == vg1_ref && lone_vgs[1] == vg2_ref) ||
                (lone_vgs[0] == vg2_ref && lone_vgs[1] == vg1_ref));

    // Vlone with smaller asize than count
    int32_t single_vg[1] = {0};
    EXPECT_THAT(Vlone(fid, single_vg, 1), Eq(2));
    EXPECT_TRUE(single_vg[0] == vg1_ref || single_vg[0] == vg2_ref);

    // Test VSlone (LoneVs1 should be lone; ChildVs is in TopVg2)
    EXPECT_THAT(VSlone(fid, nullptr, 0), Eq(1));
    EXPECT_THAT(VSlone(fid, nullptr, -1), Eq(1));
    int32_t lone_vss[10] = {0};
    const int32_t n_lone_vss = VSlone(fid, lone_vss, 10);
    EXPECT_THAT(n_lone_vss, Eq(1));
    EXPECT_THAT(lone_vss[0], Eq(vs_lone_ref));

    // VSlone with smaller asize than count
    int32_t single_vs[1] = {0};
    EXPECT_THAT(VSlone(fid, single_vs, 1), Eq(1));
  }
}

TEST(VgTest, VSisinternalAndClassQueries) {
  const std::string path = GetTempFilePath("vg_internal_classes.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  // Test VSisinternal directly
  EXPECT_THAT(VSisinternal(_HDF_ATTRIBUTE), Eq(1));
  EXPECT_THAT(VSisinternal(DIM_VALS), Eq(1));
  EXPECT_THAT(VSisinternal(DIM_VALS01), Eq(1));
  EXPECT_THAT(VSisinternal(_HDF_SDSVAR), Eq(1));
  EXPECT_THAT(VSisinternal(_HDF_CRDVAR), Eq(1));
  EXPECT_THAT(VSisinternal("_HDF_CHK_TBL_"), Eq(1));
  EXPECT_THAT(VSisinternal("_HDF_CHK_TBL_EXTENDED"), Eq(1));
  EXPECT_THAT(VSisinternal(RIGATTRNAME), Eq(1));
  EXPECT_THAT(VSisinternal(RIGATTRCLASS), Eq(1));

  EXPECT_THAT(VSisinternal("MyCustomClass"), Eq(0));
  EXPECT_THAT(VSisinternal("UserData"), Eq(0));
  EXPECT_THAT(VSisinternal(""), Eq(0));

  // Test Vsetzap
  Vsetzap();

  // Create file with user vdatas and internal vdatas
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    // Vgroup for grouping vdatas
    const int32_t vgid = Vattach(fid, -1, "w");
    ASSERT_THAT(vgid, Ne(FAIL));

    // 1. User vdata with class "UserClassA"
    const int32_t vs1 = VSattach(fid, -1, "w");
    ASSERT_THAT(vs1, Ne(FAIL));
    EXPECT_THAT(VSsetname(vs1, "UserVd1"), Eq(SUCCEED));
    EXPECT_THAT(VSsetclass(vs1, "UserClassA"), Eq(SUCCEED));
    EXPECT_THAT(VSfdefine(vs1, "F", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs1, "F"), Eq(SUCCEED));
    const int32_t val = 1;
    EXPECT_THAT(
        VSwrite(vs1, reinterpret_cast<const uint8_t*>(&val), 1, FULL_INTERLACE),
        Eq(1));
    const int32_t vs1_ref = VSQueryref(vs1);
    EXPECT_THAT(Vaddtagref(vgid, DFTAG_VH, vs1_ref), Gt(0));
    EXPECT_THAT(VSdetach(vs1), Eq(SUCCEED));

    // 2. User vdata with class "UserClassB"
    const int32_t vs2 = VSattach(fid, -1, "w");
    ASSERT_THAT(vs2, Ne(FAIL));
    EXPECT_THAT(VSsetname(vs2, "UserVd2"), Eq(SUCCEED));
    EXPECT_THAT(VSsetclass(vs2, "UserClassB"), Eq(SUCCEED));
    EXPECT_THAT(VSfdefine(vs2, "F", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs2, "F"), Eq(SUCCEED));
    EXPECT_THAT(
        VSwrite(vs2, reinterpret_cast<const uint8_t*>(&val), 1, FULL_INTERLACE),
        Eq(1));
    const int32_t vs2_ref = VSQueryref(vs2);
    EXPECT_THAT(Vaddtagref(vgid, DFTAG_VH, vs2_ref), Gt(0));
    EXPECT_THAT(VSdetach(vs2), Eq(SUCCEED));

    // 3. User vdata with NO class (empty class)
    const int32_t vs3 = VSattach(fid, -1, "w");
    ASSERT_THAT(vs3, Ne(FAIL));
    EXPECT_THAT(VSsetname(vs3, "UserVdNoClass"), Eq(SUCCEED));
    EXPECT_THAT(VSfdefine(vs3, "F", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs3, "F"), Eq(SUCCEED));
    EXPECT_THAT(
        VSwrite(vs3, reinterpret_cast<const uint8_t*>(&val), 1, FULL_INTERLACE),
        Eq(1));
    const int32_t vs3_ref = VSQueryref(vs3);
    EXPECT_THAT(Vaddtagref(vgid, DFTAG_VH, vs3_ref), Gt(0));
    EXPECT_THAT(VSdetach(vs3), Eq(SUCCEED));

    // 4. Internal vdata with class _HDF_ATTRIBUTE
    const int32_t vs4 = VSattach(fid, -1, "w");
    ASSERT_THAT(vs4, Ne(FAIL));
    EXPECT_THAT(VSsetname(vs4, "InternalAttr"), Eq(SUCCEED));
    EXPECT_THAT(VSsetclass(vs4, _HDF_ATTRIBUTE), Eq(SUCCEED));
    EXPECT_THAT(VSfdefine(vs4, "F", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs4, "F"), Eq(SUCCEED));
    EXPECT_THAT(
        VSwrite(vs4, reinterpret_cast<const uint8_t*>(&val), 1, FULL_INTERLACE),
        Eq(1));
    const int32_t vs4_ref = VSQueryref(vs4);
    EXPECT_THAT(Vaddtagref(vgid, DFTAG_VH, vs4_ref), Gt(0));
    EXPECT_THAT(VSdetach(vs4), Eq(SUCCEED));

    // 5. Internal vdata with class starting with _HDF_CHK_TBL_
    const int32_t vs5 = VSattach(fid, -1, "w");
    ASSERT_THAT(vs5, Ne(FAIL));
    EXPECT_THAT(VSsetname(vs5, "ChunkTable"), Eq(SUCCEED));
    EXPECT_THAT(VSsetclass(vs5, "_HDF_CHK_TBL_CUSTOM"), Eq(SUCCEED));
    EXPECT_THAT(VSfdefine(vs5, "F", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs5, "F"), Eq(SUCCEED));
    EXPECT_THAT(
        VSwrite(vs5, reinterpret_cast<const uint8_t*>(&val), 1, FULL_INTERLACE),
        Eq(1));
    const int32_t vs5_ref = VSQueryref(vs5);
    EXPECT_THAT(Vaddtagref(vgid, DFTAG_VH, vs5_ref), Gt(0));
    EXPECT_THAT(VSdetach(vs5), Eq(SUCCEED));

    // Test VSofclass on file id:
    // Querying with vsclass = nullptr returns count of user-created vdatas
    // (vs1, vs2, vs3)
    EXPECT_THAT(VSofclass(fid, nullptr, 0, 0, nullptr), Eq(3));
    uint16_t user_refs[10] = {0};
    EXPECT_THAT(VSofclass(fid, nullptr, 0, 10, user_refs), Eq(3));
    EXPECT_THAT(user_refs[0], Eq(vs1_ref));
    EXPECT_THAT(user_refs[1], Eq(vs2_ref));
    EXPECT_THAT(user_refs[2], Eq(vs3_ref));

    // Querying with start_vd = 1, count = 2
    uint16_t sub_refs[10] = {0};
    EXPECT_THAT(VSofclass(fid, nullptr, 1, 2, sub_refs), Eq(2));
    EXPECT_THAT(sub_refs[0], Eq(vs2_ref));
    EXPECT_THAT(sub_refs[1], Eq(vs3_ref));

    // Querying specific class "UserClassA"
    EXPECT_THAT(VSofclass(fid, "UserClassA", 0, 0, nullptr), Eq(1));
    uint16_t class_a_refs[5] = {0};
    EXPECT_THAT(VSofclass(fid, "UserClassA", 0, 5, class_a_refs), Eq(1));
    EXPECT_THAT(class_a_refs[0], Eq(vs1_ref));

    // Querying specific class _HDF_ATTRIBUTE
    EXPECT_THAT(VSofclass(fid, _HDF_ATTRIBUTE, 0, 0, nullptr), Eq(1));
    uint16_t attr_refs[5] = {0};
    EXPECT_THAT(VSofclass(fid, _HDF_ATTRIBUTE, 0, 5, attr_refs), Eq(1));
    EXPECT_THAT(attr_refs[0], Eq(vs4_ref));

    // Querying class starting with _HDF_CHK_TBL_
    EXPECT_THAT(VSofclass(fid, "_HDF_CHK_TBL_", 0, 0, nullptr), Eq(1));
    uint16_t chk_refs[5] = {0};
    EXPECT_THAT(VSofclass(fid, "_HDF_CHK_TBL_", 0, 5, chk_refs), Eq(1));
    EXPECT_THAT(chk_refs[0], Eq(vs5_ref));

    // Querying non-existent class
    EXPECT_THAT(VSofclass(fid, "NoSuchClass", 0, 0, nullptr), Eq(0));

    // start_vd beyond total count fails
    EXPECT_THAT(VSofclass(fid, nullptr, 10, 5, user_refs), Eq(FAIL));

    // Test VSofclass on vgroup id:
    EXPECT_THAT(VSofclass(vgid, nullptr, 0, 0, nullptr), Eq(3));
    uint16_t vg_user_refs[10] = {0};
    EXPECT_THAT(VSofclass(vgid, nullptr, 0, 10, vg_user_refs), Eq(3));
    EXPECT_THAT(vg_user_refs[0], Eq(vs1_ref));
    EXPECT_THAT(vg_user_refs[1], Eq(vs2_ref));
    EXPECT_THAT(vg_user_refs[2], Eq(vs3_ref));

    EXPECT_THAT(VSofclass(vgid, "UserClassB", 0, 5, sub_refs), Eq(1));
    EXPECT_THAT(sub_refs[0], Eq(vs2_ref));

    EXPECT_THAT(VSofclass(vgid, _HDF_ATTRIBUTE, 0, 5, attr_refs), Eq(1));
    EXPECT_THAT(attr_refs[0], Eq(vs4_ref));

    EXPECT_THAT(VSofclass(vgid, "_HDF_CHK_TBL_", 0, 5, chk_refs), Eq(1));
    EXPECT_THAT(chk_refs[0], Eq(vs5_ref));

    // Test VSgetvdatas (retrieves user-created vdatas only):
    // On file id:
    EXPECT_THAT(VSgetvdatas(fid, 0, 0, nullptr), Eq(3));
    uint16_t getvd_refs[10] = {0};
    EXPECT_THAT(VSgetvdatas(fid, 0, 10, getvd_refs), Eq(3));
    EXPECT_THAT(getvd_refs[0], Eq(vs1_ref));
    EXPECT_THAT(getvd_refs[1], Eq(vs2_ref));
    EXPECT_THAT(getvd_refs[2], Eq(vs3_ref));

    // Starting at 1
    EXPECT_THAT(VSgetvdatas(fid, 1, 10, getvd_refs), Eq(2));
    EXPECT_THAT(getvd_refs[0], Eq(vs2_ref));
    EXPECT_THAT(getvd_refs[1], Eq(vs3_ref));

    // On vgroup id:
    EXPECT_THAT(VSgetvdatas(vgid, 0, 0, nullptr), Eq(3));
    EXPECT_THAT(VSgetvdatas(vgid, 0, 10, getvd_refs), Eq(3));
    EXPECT_THAT(getvd_refs[0], Eq(vs1_ref));
    EXPECT_THAT(getvd_refs[1], Eq(vs2_ref));
    EXPECT_THAT(getvd_refs[2], Eq(vs3_ref));

    EXPECT_THAT(Vdetach(vgid), Eq(SUCCEED));
  }
}

TEST(VgTest, ErrorBranchesAndInvalidArguments) {
  const std::string path = GetTempFilePath("vg_errors.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
  absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

  const int32_t vsid = VSattach(fid, -1, "w");
  ASSERT_THAT(vsid, Ne(FAIL));
  absl::Cleanup detach_vs = [vsid] {
    EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));
  };

  // VSelts errors
  EXPECT_THAT(VSelts(FAIL), Eq(FAIL));
  EXPECT_THAT(VSelts(kFakeVsid), Eq(FAIL));

  // VSgetinterlace errors
  EXPECT_THAT(VSgetinterlace(FAIL), Eq(FAIL));
  EXPECT_THAT(VSgetinterlace(kFakeVsid), Eq(FAIL));

  // VSsetinterlace errors
  EXPECT_THAT(VSsetinterlace(FAIL, FULL_INTERLACE), Eq(FAIL));
  EXPECT_THAT(VSsetinterlace(kFakeVsid, FULL_INTERLACE), Eq(FAIL));
  EXPECT_THAT(VSsetinterlace(vsid, 999), Eq(FAIL));  // Invalid interlace

  // VSgetfields errors
  char flds[64];
  EXPECT_THAT(VSgetfields(FAIL, flds), Eq(FAIL));
  EXPECT_THAT(VSgetfields(kFakeVsid, flds), Eq(FAIL));
  EXPECT_THAT(VSgetfields(vsid, nullptr), Eq(FAIL));

  // VSfexist errors
  char fld_f[] = "F";
  EXPECT_THAT(VSfexist(FAIL, fld_f), Eq(FAIL));
  EXPECT_THAT(VSfexist(kFakeVsid, fld_f), Eq(FAIL));

  // VSsizeof errors
  EXPECT_THAT(VSsizeof(FAIL, fld_f), Eq(FAIL));
  EXPECT_THAT(VSsizeof(kFakeVsid, fld_f), Eq(FAIL));

  // VSdump errors
  VSdump(FAIL);
  VSdump(kFakeVsid);

  // VSsetname errors
  EXPECT_THAT(VSsetname(FAIL, "Name"), Eq(FAIL));
  EXPECT_THAT(VSsetname(kFakeVsid, "Name"), Eq(FAIL));
  EXPECT_THAT(VSsetname(vsid, nullptr), Eq(FAIL));
  const std::string too_long_name(VSNAMELENMAX + 10, 'A');
  EXPECT_THAT(VSsetname(vsid, too_long_name.c_str()), Eq(SUCCEED));

  // VSsetclass errors
  EXPECT_THAT(VSsetclass(FAIL, "Class"), Eq(FAIL));
  EXPECT_THAT(VSsetclass(kFakeVsid, "Class"), Eq(FAIL));
  EXPECT_THAT(VSsetclass(vsid, nullptr), Eq(FAIL));
  const std::string too_long_class(VSNAMELENMAX + 10, 'B');
  EXPECT_THAT(VSsetclass(vsid, too_long_class.c_str()), Eq(SUCCEED));

  // VSgetname errors
  char name[VSNAMELENMAX + 1];
  EXPECT_THAT(VSgetname(FAIL, name), Eq(FAIL));
  EXPECT_THAT(VSgetname(kFakeVsid, name), Eq(FAIL));
  EXPECT_THAT(VSgetname(vsid, nullptr), Eq(FAIL));

  // VSgetclass errors
  char cl[VSNAMELENMAX + 1];
  EXPECT_THAT(VSgetclass(FAIL, cl), Eq(FAIL));
  EXPECT_THAT(VSgetclass(kFakeVsid, cl), Eq(FAIL));
  EXPECT_THAT(VSgetclass(vsid, nullptr), Eq(FAIL));

  // VSinquire errors
  int32_t nelt, interlace, eltsize;
  EXPECT_THAT(VSinquire(FAIL, &nelt, &interlace, flds, &eltsize, name),
              Eq(FAIL));
  EXPECT_THAT(VSinquire(kFakeVsid, &nelt, &interlace, flds, &eltsize, name),
              Eq(FAIL));

  // VSlone and Vlone errors
  int32_t refs[5];
  EXPECT_THAT(VSlone(FAIL, refs, 5), Eq(0));
  EXPECT_THAT(VSlone(kFakeFid, refs, 5), Eq(0));
  EXPECT_THAT(Vlone(FAIL, refs, 5), Eq(0));
  EXPECT_THAT(Vlone(kFakeFid, refs, 5), Eq(0));

  // Vfind and VSfind errors
  EXPECT_THAT(Vfind(FAIL, "Name"), Eq(0));
  EXPECT_THAT(Vfind(kFakeFid, "Name"), Eq(0));
  EXPECT_THAT(Vfind(fid, nullptr), Eq(FAIL));
  EXPECT_THAT(VSfind(FAIL, "Name"), Eq(0));
  EXPECT_THAT(VSfind(kFakeFid, "Name"), Eq(0));
  EXPECT_THAT(VSfind(fid, nullptr), Eq(FAIL));

  // Vfindclass and VSfindclass errors
  EXPECT_THAT(Vfindclass(FAIL, "Class"), Eq(0));
  EXPECT_THAT(Vfindclass(kFakeFid, "Class"), Eq(0));
  EXPECT_THAT(Vfindclass(fid, nullptr), Eq(FAIL));
  EXPECT_THAT(VSfindclass(FAIL, "Class"), Eq(0));
  EXPECT_THAT(VSfindclass(kFakeFid, "Class"), Eq(0));
  EXPECT_THAT(VSfindclass(fid, nullptr), Eq(FAIL));

  // Empty string in VSfexist (ac < 1)
  char empty_str[] = "";
  EXPECT_THAT(VSfexist(vsid, empty_str), Eq(FAIL));

  // VSsetblocksize and VSsetnumblocks errors
  EXPECT_THAT(VSsetblocksize(FAIL, 512), Eq(FAIL));
  EXPECT_THAT(VSsetblocksize(kFakeVsid, 512), Eq(FAIL));
  EXPECT_THAT(VSsetblocksize(vsid, -1), Eq(SUCCEED));

  EXPECT_THAT(VSsetnumblocks(FAIL, 2), Eq(FAIL));
  EXPECT_THAT(VSsetnumblocks(kFakeVsid, 2), Eq(FAIL));
  EXPECT_THAT(VSsetnumblocks(vsid, -1), Eq(SUCCEED));

  EXPECT_THAT(VSgetblockinfo(FAIL, &nelt, &eltsize), Eq(FAIL));
  EXPECT_THAT(VSgetblockinfo(kFakeVsid, &nelt, &eltsize), Eq(FAIL));

  // VSofclass and VSgetvdatas errors
  uint16_t u16_refs[5];
  EXPECT_THAT(VSofclass(FAIL, nullptr, 0, 5, u16_refs), Eq(FAIL));
  EXPECT_THAT(VSofclass(vsid, nullptr, 0, 5, u16_refs),
              Eq(FAIL));  // Bad id type
  EXPECT_THAT(VSofclass(fid, nullptr, 0, 0, u16_refs),
              Eq(FAIL));  // Non-null refarray with count 0

  EXPECT_THAT(VSgetvdatas(FAIL, 0, 5, u16_refs), Eq(FAIL));
  EXPECT_THAT(VSgetvdatas(vsid, 0, 5, u16_refs), Eq(FAIL));  // Bad id type
  EXPECT_THAT(VSgetvdatas(fid, 0, 0, u16_refs),
              Eq(FAIL));  // Non-null refarray with count 0
}

}  // namespace
}  // namespace hdf4
