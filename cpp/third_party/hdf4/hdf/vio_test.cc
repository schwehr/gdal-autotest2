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
#include "third_party/hdf4/hdf/src/vg_priv.h"

namespace hdf4 {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::Ne;
using ::testing::NotNull;
using ::testing::StrEq;

constexpr int32_t kFakeVsid = (4 << 28) | 0x1234;
constexpr int32_t kFakeVgid = (3 << 28) | 0x1234;
constexpr int32_t kFakeFid = (1 << 28) | 0x1234;

std::string GetTempFilePath(std::string_view filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

TEST(VioTest, AttachCreateWriteAndDetach) {
  const std::string path = GetTempFilePath("vio_attach_write.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
  absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

  // Create new vdata with VSattach(fid, -1, "w")
  const int32_t vsid = VSattach(fid, -1, "w");
  ASSERT_THAT(vsid, Ne(FAIL));

  // Query tag, ref, version before writing
  EXPECT_THAT(VSQuerytag(vsid), Eq(DFTAG_VH));
  const int32_t vs_ref = VSQueryref(vsid);
  EXPECT_THAT(vs_ref, Gt(0));
  EXPECT_THAT(VSgetversion(vsid), Eq(VSET_VERSION));

  // Naming and classes
  EXPECT_THAT(VSsetname(vsid, "SensorsData"), Eq(SUCCEED));
  EXPECT_THAT(VSsetclass(vsid, "TelemetryClass"), Eq(SUCCEED));

  // Define fields: 1 int32, 2 float32s
  EXPECT_THAT(VSfdefine(vsid, "PacketID", DFNT_INT32, 1), Eq(SUCCEED));
  EXPECT_THAT(VSfdefine(vsid, "Coords", DFNT_FLOAT32, 2), Eq(SUCCEED));
  EXPECT_THAT(VSsetfields(vsid, "PacketID,Coords"), Eq(SUCCEED));

  // Verify writelist
  DYN_VWRITELIST* wlist = vswritelist(vsid);
  ASSERT_THAT(wlist, NotNull());
  EXPECT_THAT(wlist->n, Eq(2));
  EXPECT_THAT(wlist->ivsize, Eq(sizeof(int32_t) + 2 * sizeof(float)));

  // Write 2 records
  struct Record {
    int32_t id;
    float coords[2];
  };
  const Record records[2] = {
      {100, {1.5f, 2.5f}},
      {101, {3.5f, 4.5f}},
  };
  EXPECT_THAT(VSwrite(vsid, reinterpret_cast<const uint8_t*>(records), 2,
                      FULL_INTERLACE),
              Eq(2));

  // Detach writes out VSDESC (DFTAG_VH) and data (DFTAG_VS)
  EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));
}

TEST(VioTest, ReopenReadAndSeek) {
  const std::string path = GetTempFilePath("vio_reopen_read.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  int32_t saved_ref = FAIL;
  struct Record {
    int32_t val;
  };

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    const int32_t vsid = VSattach(fid, -1, "w");
    ASSERT_THAT(vsid, Ne(FAIL));
    saved_ref = VSQueryref(vsid);

    EXPECT_THAT(VSfdefine(vsid, "Value", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vsid, "Value"), Eq(SUCCEED));

    const Record records[3] = {{10}, {20}, {30}};
    EXPECT_THAT(VSwrite(vsid, reinterpret_cast<const uint8_t*>(records), 3,
                        FULL_INTERLACE),
                Eq(3));
    EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));
  }

  // Reopen for read
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    // vexistvs
    EXPECT_THAT(vexistvs(fid, static_cast<uint16_t>(saved_ref)), Eq(1));
    EXPECT_THAT(vexistvs(fid, 9999), Eq(FAIL));

    // Attach existing vdata for read
    const int32_t vsid = VSattach(fid, saved_ref, "r");
    ASSERT_THAT(vsid, Ne(FAIL));

    // Second attach in read mode (hits lines 896-904)
    const int32_t r2 = VSattach(fid, saved_ref, "r");
    ASSERT_THAT(r2, Ne(FAIL));
    EXPECT_THAT(VSdetach(r2), Eq(SUCCEED));
    EXPECT_THAT(HAremove_atom(r2), NotNull());

    EXPECT_THAT(VSQuerytag(vsid), Eq(DFTAG_VH));
    EXPECT_THAT(VSQueryref(vsid), Eq(saved_ref));
    EXPECT_THAT(VSgetversion(vsid), Eq(VSET_VERSION));

    // Seek and read
    EXPECT_THAT(VSsetfields(vsid, "Value"), Eq(SUCCEED));
    Record buf[3] = {};
    EXPECT_THAT(
        VSread(vsid, reinterpret_cast<uint8_t*>(buf), 3, FULL_INTERLACE),
        Eq(3));
    EXPECT_THAT(buf[0].val, Eq(10));
    EXPECT_THAT(buf[1].val, Eq(20));
    EXPECT_THAT(buf[2].val, Eq(30));

    EXPECT_THAT(VSseek(vsid, 1), Eq(1));
    Record single_buf = {};
    EXPECT_THAT(VSread(vsid, reinterpret_cast<uint8_t*>(&single_buf), 1,
                       FULL_INTERLACE),
                Eq(1));
    EXPECT_THAT(single_buf.val, Eq(20));

    EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));
  }
}

TEST(VioTest, VdataAttributesLifecycle) {
  const std::string path = GetTempFilePath("vio_attrs.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  int32_t vs_ref = FAIL;
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    const int32_t vsid = VSattach(fid, -1, "w");
    ASSERT_THAT(vsid, Ne(FAIL));
    vs_ref = VSQueryref(vsid);

    EXPECT_THAT(VSsetname(vsid, "AttrVdata"), Eq(SUCCEED));
    EXPECT_THAT(VSsetclass(vsid, "AttrClass"), Eq(SUCCEED));

    EXPECT_THAT(VSfdefine(vsid, "FieldX", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vsid, "FieldX"), Eq(SUCCEED));

    const int32_t x_val = 999;
    EXPECT_THAT(VSwrite(vsid, reinterpret_cast<const uint8_t*>(&x_val), 1,
                        FULL_INTERLACE),
                Eq(1));

    // Add Vdata attribute and field attribute
    const int32_t vdata_attr = 42;
    EXPECT_THAT(
        VSsetattr(vsid, _HDF_VDATA, "GlobalAttr", DFNT_INT32, 1, &vdata_attr),
        Eq(SUCCEED));
    const char fld_attr[] = "field_comment";
    EXPECT_THAT(
        VSsetattr(vsid, 0, "FieldAttr", DFNT_CHAR8, sizeof(fld_attr), fld_attr),
        Eq(SUCCEED));

    // Version with attributes is VSET_NEW_VERSION (4)
    EXPECT_THAT(VSgetversion(vsid), Eq(VSET_NEW_VERSION));

    // Detach encodes attributes into VSDESC
    EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));
  }

  // Reopen and verify unpacked attributes
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_RDWR, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    // Attach existing vdata for write to modify header
    const int32_t vsid = VSattach(fid, vs_ref, "w");
    ASSERT_THAT(vsid, Ne(FAIL));

    EXPECT_THAT(VSgetversion(vsid), Eq(VSET_NEW_VERSION));
    EXPECT_THAT(VSnattrs(vsid), Eq(2));
    EXPECT_THAT(VSfindattr(vsid, _HDF_VDATA, "GlobalAttr"), Ge(0));
    EXPECT_THAT(VSfindattr(vsid, 0, "FieldAttr"), Ge(0));

    // Change name to a longer name to set vs->new_h_sz = 1
    // and trigger HDreuse_tagref in VSdetach (lines 1043-1051)
    EXPECT_THAT(
        VSsetname(vsid, "MuchLongerAttrVdataNameToTriggerNewHeaderSize"),
        Eq(SUCCEED));

    // Append record to trigger write in VSdetach
    const int32_t x2 = 1000;
    EXPECT_THAT(
        VSwrite(vsid, reinterpret_cast<const uint8_t*>(&x2), 1, FULL_INTERLACE),
        Eq(1));

    EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));
  }
}

TEST(VioTest, ZeroFieldsVdata) {
  const std::string path = GetTempFilePath("vio_zero_fields.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  int32_t vs_ref = FAIL;
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    // Create vdata without defining any fields (wlist.n == 0)
    const int32_t vsid = VSattach(fid, -1, "w");
    ASSERT_THAT(vsid, Ne(FAIL));
    vs_ref = VSQueryref(vsid);

    EXPECT_THAT(VSsetname(vsid, "ZeroFieldVdata"), Eq(SUCCEED));
    EXPECT_THAT(VSsetclass(vsid, "ZeroFieldClass"), Eq(SUCCEED));

    EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));
  }

  // Reopen and unpack 0-field vdata
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    EXPECT_THAT(vexistvs(fid, static_cast<uint16_t>(vs_ref)), Eq(1));
    const int32_t vsid = VSattach(fid, vs_ref, "r");
    ASSERT_THAT(vsid, Ne(FAIL));
    EXPECT_THAT(VSQueryref(vsid), Eq(vs_ref));
    EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));
  }
}

TEST(VioTest, VSappendableModes) {
  const std::string path = GetTempFilePath("vio_appendable.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
  absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

  const int32_t vsid = VSattach(fid, -1, "w");
  ASSERT_THAT(vsid, Ne(FAIL));

  // VSappendable when aid == 0
  EXPECT_THAT(VSappendable(vsid, 0), Eq(SUCCEED));

  EXPECT_THAT(VSfdefine(vsid, "Data", DFNT_INT32, 1), Eq(SUCCEED));
  EXPECT_THAT(VSsetfields(vsid, "Data"), Eq(SUCCEED));

  const int32_t val = 55;
  EXPECT_THAT(
      VSwrite(vsid, reinterpret_cast<const uint8_t*>(&val), 1, FULL_INTERLACE),
      Eq(1));

  // VSappendable when aid != 0
  EXPECT_THAT(VSappendable(vsid, 0), Eq(SUCCEED));

  EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));
}

TEST(VioTest, VSgetidNavigation) {
  const std::string path = GetTempFilePath("vio_getid.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  // Empty file with no vdatas returns FAIL on VSgetid(fid, -1)
  {
    const int32_t empty_fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(empty_fid, Ne(FAIL));
    ASSERT_THAT(Vstart(empty_fid), Eq(SUCCEED));
    EXPECT_THAT(VSgetid(empty_fid, -1), Eq(FAIL));
    EXPECT_THAT(Vend(empty_fid), Eq(SUCCEED));
    EXPECT_THAT(Hclose(empty_fid), Eq(SUCCEED));
  }

  // Create 3 vdatas
  int32_t ref1 = FAIL, ref2 = FAIL, ref3 = FAIL;
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_RDWR, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    const int32_t vs1 = VSattach(fid, -1, "w");
    ASSERT_THAT(vs1, Ne(FAIL));
    ref1 = VSQueryref(vs1);
    EXPECT_THAT(VSdetach(vs1), Eq(SUCCEED));

    const int32_t vs2 = VSattach(fid, -1, "w");
    ASSERT_THAT(vs2, Ne(FAIL));
    ref2 = VSQueryref(vs2);
    EXPECT_THAT(VSdetach(vs2), Eq(SUCCEED));

    const int32_t vs3 = VSattach(fid, -1, "w");
    ASSERT_THAT(vs3, Ne(FAIL));
    ref3 = VSQueryref(vs3);
    EXPECT_THAT(VSdetach(vs3), Eq(SUCCEED));

    // Iterate through vdatas using VSgetid
    const int32_t first = VSgetid(fid, -1);
    EXPECT_THAT(first, Ne(FAIL));
    const int32_t second = VSgetid(fid, first);
    EXPECT_THAT(second, Ne(FAIL));
    const int32_t third = VSgetid(fid, second);
    EXPECT_THAT(third, Ne(FAIL));

    // End of iteration returns FAIL
    EXPECT_THAT(VSgetid(fid, third), Eq(FAIL));
    // Non-existent id returns FAIL
    EXPECT_THAT(VSgetid(fid, 99999), Eq(FAIL));
    // vsid < -1 returns FAIL
    EXPECT_THAT(VSgetid(fid, -2), Eq(FAIL));
  }
}

TEST(VioTest, VSdeleteLifecycle) {
  const std::string path = GetTempFilePath("vio_delete.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  int32_t vs1_ref = FAIL, vs2_ref = FAIL;
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    const int32_t vs1 = VSattach(fid, -1, "w");
    ASSERT_THAT(vs1, Ne(FAIL));
    vs1_ref = VSQueryref(vs1);
    EXPECT_THAT(VSfdefine(vs1, "Val", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs1, "Val"), Eq(SUCCEED));
    const int32_t val1 = 11;
    EXPECT_THAT(VSwrite(vs1, reinterpret_cast<const uint8_t*>(&val1), 1,
                        FULL_INTERLACE),
                Eq(1));
    EXPECT_THAT(VSdetach(vs1), Eq(SUCCEED));

    const int32_t vs2 = VSattach(fid, -1, "w");
    ASSERT_THAT(vs2, Ne(FAIL));
    vs2_ref = VSQueryref(vs2);
    EXPECT_THAT(VSfdefine(vs2, "Val", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs2, "Val"), Eq(SUCCEED));
    const int32_t val2 = 22;
    EXPECT_THAT(VSwrite(vs2, reinterpret_cast<const uint8_t*>(&val2), 1,
                        FULL_INTERLACE),
                Eq(1));
    EXPECT_THAT(VSdetach(vs2), Eq(SUCCEED));

    EXPECT_THAT(vexistvs(fid, static_cast<uint16_t>(vs1_ref)), Eq(1));
    EXPECT_THAT(vexistvs(fid, static_cast<uint16_t>(vs2_ref)), Eq(1));

    // Delete vs2
    EXPECT_THAT(VSdelete(fid, vs2_ref), Eq(SUCCEED));
    EXPECT_THAT(vexistvs(fid, static_cast<uint16_t>(vs2_ref)), Eq(FAIL));
    EXPECT_THAT(vexistvs(fid, static_cast<uint16_t>(vs1_ref)), Eq(1));

    // VSdelete error cases
    EXPECT_THAT(VSdelete(fid, 99999), Eq(FAIL));
    EXPECT_THAT(VSdelete(fid, -2), Eq(FAIL));
    EXPECT_THAT(VSdelete(FAIL, vs1_ref), Eq(FAIL));
    EXPECT_THAT(VSdelete(kFakeFid, vs1_ref), Eq(FAIL));
  }

  // Reopen and verify vs2 remains deleted
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    EXPECT_THAT(vexistvs(fid, static_cast<uint16_t>(vs1_ref)), Eq(1));
    EXPECT_THAT(vexistvs(fid, static_cast<uint16_t>(vs2_ref)), Eq(FAIL));
  }
}

TEST(VioTest, VSPhshutdownLifecycle) {
  const std::string path = GetTempFilePath("vio_shutdown.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));

    // First vdata: small header
    const int32_t vs1 = VSattach(fid, -1, "w");
    ASSERT_THAT(vs1, Ne(FAIL));
    EXPECT_THAT(VSsetname(vs1, "S"), Eq(SUCCEED));
    EXPECT_THAT(VSdetach(vs1), Eq(SUCCEED));

    // Second vdata: larger header to trigger buffer resize in VSPgetinfo
    const int32_t vs2 = VSattach(fid, -1, "w");
    ASSERT_THAT(vs2, Ne(FAIL));
    EXPECT_THAT(VSsetname(vs2, "VeryLongVdataNameToExceedSmallBufferLength"),
                Eq(SUCCEED));
    EXPECT_THAT(VSfdefine(vs2, "Fld1", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSfdefine(vs2, "Fld2", DFNT_FLOAT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs2, "Fld1,Fld2"), Eq(SUCCEED));
    EXPECT_THAT(VSdetach(vs2), Eq(SUCCEED));

    EXPECT_THAT(Vend(fid), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Call VSPhshutdown directly to clear free lists and buffers
  EXPECT_THAT(VSPhshutdown(), Eq(SUCCEED));

  // Reopening after shutdown: VSPgetinfo loads vs1 (allocating small Vhbuf),
  // then loads vs2 (larger vh_length > Vhbufsize), hitting line 714
  // free(Vhbuf).
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    EXPECT_THAT(Vstart(fid), Eq(SUCCEED));
    EXPECT_THAT(Vend(fid), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(VioTest, DoubleAttachWriteError) {
  const std::string path = GetTempFilePath("vio_double_attach.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
  absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

  const int32_t vs = VSattach(fid, -1, "w");
  ASSERT_THAT(vs, Ne(FAIL));
  const int32_t vs_ref = VSQueryref(vs);
  EXPECT_THAT(VSdetach(vs), Eq(SUCCEED));

  // Attach existing vdata for write
  const int32_t vs_w1 = VSattach(fid, vs_ref, "w");
  ASSERT_THAT(vs_w1, Ne(FAIL));

  // Attaching same vdata again in write mode returns FAIL (DFE_BADATTACH)
  EXPECT_THAT(VSattach(fid, vs_ref, "w"), Eq(FAIL));

  EXPECT_THAT(VSdetach(vs_w1), Eq(SUCCEED));
}

TEST(VioTest, ErrorBranchesAndInvalidArguments) {
  const std::string path = GetTempFilePath("vio_errors.hdf");
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

  // VSattach errors
  EXPECT_THAT(VSattach(FAIL, -1, "w"), Eq(FAIL));
  EXPECT_THAT(VSattach(fid, -2, "w"), Eq(FAIL));
  EXPECT_THAT(VSattach(kFakeFid, -1, "w"), Eq(FAIL));
  EXPECT_THAT(VSattach(fid, -1, "r"), Eq(FAIL));
  EXPECT_THAT(VSattach(fid, 1, "invalid_mode"), Eq(FAIL));
  EXPECT_THAT(VSattach(fid, 9999, "r"), Eq(FAIL));
  EXPECT_THAT(VSattach(fid, 9999, "w"), Eq(FAIL));

  // VSdetach errors
  EXPECT_THAT(VSdetach(FAIL), Eq(FAIL));
  EXPECT_THAT(VSdetach(kFakeVsid), Eq(FAIL));
  EXPECT_THAT(VSdetach(kFakeVgid), Eq(FAIL));

  // VSappendable errors
  EXPECT_THAT(VSappendable(FAIL, 0), Eq(FAIL));
  EXPECT_THAT(VSappendable(kFakeVsid, 0), Eq(FAIL));

  // VSQuerytag and VSQueryref errors
  EXPECT_THAT(VSQuerytag(FAIL), Eq(FAIL));
  EXPECT_THAT(VSQuerytag(kFakeVsid), Eq(FAIL));
  EXPECT_THAT(VSQueryref(FAIL), Eq(FAIL));
  EXPECT_THAT(VSQueryref(kFakeVsid), Eq(FAIL));

  // vswritelist errors
  EXPECT_THAT(vswritelist(FAIL), testing::IsNull());
  EXPECT_THAT(vswritelist(kFakeVsid), testing::IsNull());

  // VSgetversion errors
  EXPECT_THAT(VSgetversion(FAIL), Eq(0));
  EXPECT_THAT(VSgetversion(kFakeVsid), Eq(0));

  // vexistvs errors
  EXPECT_THAT(vexistvs(FAIL, 1), Eq(FAIL));
  EXPECT_THAT(vexistvs(kFakeFid, 1), Eq(FAIL));
}

}  // namespace
}  // namespace hdf4
