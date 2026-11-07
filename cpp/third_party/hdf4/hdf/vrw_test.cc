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

// Tests for Vdata read, write, and seek operations
// (third_party/hdf4/hdf/src/vrw.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/vrw.c

#include <cstdint>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"
#include "third_party/hdf4/hdf/src/vg.h"

namespace hdf4 {
namespace {

using ::testing::ElementsAre;

std::string GetTempFilePath(const std::string& name) {
  return (std::filesystem::path(::testing::TempDir()) / name).string();
}

class VrwTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }

  void TearDown() override {
    VSPshutdown();
    HEclear();
  }
};

TEST_F(VrwTest, ShutdownCleansBuffer) { EXPECT_EQ(SUCCEED, VSPshutdown()); }

TEST_F(VrwTest, SeekErrors) {
  const std::string path = GetTempFilePath("vrw_seek_errors.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

  // 1. Invalid key (not a VSID).
  HEclear();
  EXPECT_EQ(FAIL, VSseek(fid, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // 2. Unallocated / closed vkey.
  HEclear();
  EXPECT_EQ(FAIL, VSseek(999999, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  const int32 vskey = VSattach(fid, -1, "w");
  ASSERT_NE(FAIL, vskey);
  absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

  // 3. Negative element position.
  HEclear();
  EXPECT_EQ(FAIL, VSseek(vskey, -1));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // 4. Seek on vdata with 0 fields.
  HEclear();
  EXPECT_EQ(FAIL, VSseek(vskey, 0));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));
}

TEST_F(VrwTest, ReadWriteErrors) {
  const std::string path = GetTempFilePath("vrw_rw_errors.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

  const int32 vskey_w = VSattach(fid, -1, "w");
  ASSERT_NE(FAIL, vskey_w);
  absl::Cleanup detach_w = [vskey_w] { EXPECT_EQ(SUCCEED, VSdetach(vskey_w)); };

  uint8 dummy_buf[16] = {0};

  // VSwrite invalid group.
  HEclear();
  EXPECT_EQ(FAIL, VSwrite(fid, dummy_buf, 1, FULL_INTERLACE));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // VSwrite nelt <= 0.
  HEclear();
  EXPECT_EQ(FAIL, VSwrite(vskey_w, dummy_buf, 0, FULL_INTERLACE));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VSwrite(vskey_w, dummy_buf, -5, FULL_INTERLACE));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // VSwrite with no fields defined.
  HEclear();
  EXPECT_EQ(FAIL, VSwrite(vskey_w, dummy_buf, 1, FULL_INTERLACE));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));

  // Define field and set invalid interlace.
  ASSERT_EQ(SUCCEED, VSfdefine(vskey_w, "F1", DFNT_INT32, 1));
  ASSERT_EQ(SUCCEED, VSsetfields(vskey_w, "F1"));

  HEclear();
  EXPECT_EQ(FAIL, VSwrite(vskey_w, dummy_buf, 1, 999));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // VSread invalid group.
  HEclear();
  EXPECT_EQ(FAIL, VSread(fid, dummy_buf, 1, FULL_INTERLACE));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // VSread on write-mode vdata before any vertices are written (nvertices ==
  // 0).
  HEclear();
  EXPECT_EQ(FAIL, VSread(vskey_w, dummy_buf, 1, FULL_INTERLACE));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // VSread on write-mode vdata before any vertices are written (nvertices ==
  // 0).
  HEclear();
  EXPECT_EQ(FAIL, VSread(vskey_w, dummy_buf, 1, FULL_INTERLACE));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
}

TEST_F(VrwTest, SingleFieldReadWriteAndSeek) {
  const std::string path = GetTempFilePath("vrw_single_field.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  int32 vs_ref = -1;

  // 1. Write single-field vdata (Case E).
  {
    const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ASSERT_EQ(SUCCEED, Vstart(fid));
    absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

    const int32 vskey = VSattach(fid, -1, "w");
    ASSERT_NE(FAIL, vskey);
    absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

    ASSERT_EQ(SUCCEED, VSfdefine(vskey, "Temperature", DFNT_INT32, 1));
    ASSERT_EQ(SUCCEED, VSsetfields(vskey, "Temperature"));

    // Write chunk 1: 5 elements.
    const std::vector<int32> data1 = {10, 20, 30, 40, 50};
    EXPECT_EQ(5, VSwrite(vskey, reinterpret_cast<const uint8*>(data1.data()), 5,
                         FULL_INTERLACE));

    // Write chunk 2: 3 elements (tests growth and buffer reuse).
    const std::vector<int32> data2 = {60, 70, 80};
    EXPECT_EQ(3, VSwrite(vskey, reinterpret_cast<const uint8*>(data2.data()), 3,
                         FULL_INTERLACE));

    vs_ref = VSQueryref(vskey);
    ASSERT_GT(vs_ref, 0);
  }

  // 2. Read back single-field vdata and test seeking.
  {
    const int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ASSERT_EQ(SUCCEED, Vstart(fid));
    absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

    const int32 vskey = VSattach(fid, vs_ref, "r");
    ASSERT_NE(FAIL, vskey);
    absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

    ASSERT_EQ(SUCCEED, VSsetfields(vskey, "Temperature"));

    // Read first 4 elements.
    std::vector<int32> read_buf1(4, 0);
    EXPECT_EQ(4, VSread(vskey, reinterpret_cast<uint8*>(read_buf1.data()), 4,
                        FULL_INTERLACE));
    EXPECT_THAT(read_buf1, ElementsAre(10, 20, 30, 40));

    // Seek to element index 5.
    EXPECT_EQ(5, VSseek(vskey, 5));

    // Read remaining 3 elements.
    std::vector<int32> read_buf2(3, 0);
    EXPECT_EQ(3, VSread(vskey, reinterpret_cast<uint8*>(read_buf2.data()), 3,
                        FULL_INTERLACE));
    EXPECT_THAT(read_buf2, ElementsAre(60, 70, 80));

    // Seek back to element 0 and read all 8.
    EXPECT_EQ(0, VSseek(vskey, 0));
    std::vector<int32> read_all(8, 0);
    EXPECT_EQ(8, VSread(vskey, reinterpret_cast<uint8*>(read_all.data()), 8,
                        FULL_INTERLACE));
    EXPECT_THAT(read_all, ElementsAre(10, 20, 30, 40, 50, 60, 70, 80));
  }
}

TEST_F(VrwTest, MultiFieldCaseC_FullFull) {
  const std::string path = GetTempFilePath("vrw_case_c.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  int32 vs_ref = -1;

  // Case C: user FULL_INTERLACE & vdata FULL_INTERLACE.
  {
    const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ASSERT_EQ(SUCCEED, Vstart(fid));
    absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

    const int32 vskey = VSattach(fid, -1, "w");
    ASSERT_NE(FAIL, vskey);
    absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

    ASSERT_EQ(SUCCEED, VSsetinterlace(vskey, FULL_INTERLACE));
    ASSERT_EQ(SUCCEED, VSfdefine(vskey, "ValA", DFNT_INT32, 1));
    ASSERT_EQ(SUCCEED, VSfdefine(vskey, "ValB", DFNT_INT32, 1));
    ASSERT_EQ(SUCCEED, VSsetfields(vskey, "ValA,ValB"));

    struct Record {
      int32 a;
      int32 b;
    };
    std::vector<Record> records = {
        {1, 100},
        {2, 200},
        {3, 300},
    };

    EXPECT_EQ(3, VSwrite(vskey, reinterpret_cast<const uint8*>(records.data()),
                         3, FULL_INTERLACE));
    vs_ref = VSQueryref(vskey);
  }

  {
    const int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ASSERT_EQ(SUCCEED, Vstart(fid));
    absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

    const int32 vskey = VSattach(fid, vs_ref, "r");
    ASSERT_NE(FAIL, vskey);
    absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

    // VSwrite on read-only vdata fails with DFE_BADACC (covers line 491).
    uint8 dummy_record[8] = {0};
    HEclear();
    EXPECT_EQ(FAIL, VSwrite(vskey, dummy_record, 1, FULL_INTERLACE));
    EXPECT_EQ(DFE_BADACC, HEvalue(1));

    ASSERT_EQ(SUCCEED, VSsetfields(vskey, "ValA,ValB"));

    struct Record {
      int32 a;
      int32 b;
    };
    std::vector<Record> read_records(3, {0, 0});
    EXPECT_EQ(3, VSread(vskey, reinterpret_cast<uint8*>(read_records.data()), 3,
                        FULL_INTERLACE));
    EXPECT_EQ(1, read_records[0].a);
    EXPECT_EQ(100, read_records[0].b);
    EXPECT_EQ(2, read_records[1].a);
    EXPECT_EQ(200, read_records[1].b);
    EXPECT_EQ(3, read_records[2].a);
    EXPECT_EQ(300, read_records[2].b);
  }
}

TEST_F(VrwTest, MultiFieldCaseA_UserNoneVdataFull) {
  const std::string path = GetTempFilePath("vrw_case_a.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  int32 vs_ref = -1;

  // Case A: user NO_INTERLACE & vdata FULL_INTERLACE.
  {
    const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ASSERT_EQ(SUCCEED, Vstart(fid));
    absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

    const int32 vskey = VSattach(fid, -1, "w");
    ASSERT_NE(FAIL, vskey);
    absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

    ASSERT_EQ(SUCCEED, VSsetinterlace(vskey, FULL_INTERLACE));
    ASSERT_EQ(SUCCEED, VSfdefine(vskey, "X", DFNT_INT16, 1));
    ASSERT_EQ(SUCCEED, VSfdefine(vskey, "Y", DFNT_INT16, 1));
    ASSERT_EQ(SUCCEED, VSsetfields(vskey, "X,Y"));

    // User buffer in NO_INTERLACE format: all X values followed by all Y
    // values. 4 elements: X=[1,2,3,4], Y=[10,20,30,40]
    std::vector<int16> non_interlaced_buf = {1, 2, 3, 4, 10, 20, 30, 40};
    EXPECT_EQ(
        4, VSwrite(vskey,
                   reinterpret_cast<const uint8*>(non_interlaced_buf.data()), 4,
                   NO_INTERLACE));
    vs_ref = VSQueryref(vskey);
  }

  {
    const int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ASSERT_EQ(SUCCEED, Vstart(fid));
    absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

    const int32 vskey = VSattach(fid, vs_ref, "r");
    ASSERT_NE(FAIL, vskey);
    absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

    ASSERT_EQ(SUCCEED, VSsetfields(vskey, "X,Y"));

    // Reset buffer size to test dynamic allocation in Case A/B/D.
    VSPshutdown();

    // Also test invalid interlace in VSread on valid vdata (line 209).
    uint8 dummy[8] = {0};
    HEclear();
    EXPECT_EQ(FAIL, VSread(vskey, dummy, 4, 999));
    EXPECT_EQ(DFE_ARGS, HEvalue(1));

    std::vector<int16> read_buf(8, 0);
    EXPECT_EQ(4, VSread(vskey, reinterpret_cast<uint8*>(read_buf.data()), 4,
                        NO_INTERLACE));
    EXPECT_THAT(read_buf, ElementsAre(1, 2, 3, 4, 10, 20, 30, 40));
  }
}

TEST_F(VrwTest, MultiFieldCaseB_UserNoneVdataNone) {
  const std::string path = GetTempFilePath("vrw_case_b.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  int32 vs_ref = -1;

  // Case B: user NO_INTERLACE & vdata NO_INTERLACE.
  {
    const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ASSERT_EQ(SUCCEED, Vstart(fid));
    absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

    const int32 vskey = VSattach(fid, -1, "w");
    ASSERT_NE(FAIL, vskey);
    absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

    ASSERT_EQ(SUCCEED, VSsetinterlace(vskey, NO_INTERLACE));
    ASSERT_EQ(SUCCEED, VSfdefine(vskey, "A", DFNT_INT32, 1));
    ASSERT_EQ(SUCCEED, VSfdefine(vskey, "B", DFNT_INT32, 1));
    ASSERT_EQ(SUCCEED, VSsetfields(vskey, "A,B"));

    // User buffer: 3 elements of A (int32), followed by 3 elements of B
    // (int32).
    std::vector<int32> buffer = {5, 6, 7, 500, 600, 700};
    EXPECT_EQ(3, VSwrite(vskey, reinterpret_cast<const uint8*>(buffer.data()),
                         3, NO_INTERLACE));
    vs_ref = VSQueryref(vskey);
  }

  {
    const int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ASSERT_EQ(SUCCEED, Vstart(fid));
    absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

    const int32 vskey = VSattach(fid, vs_ref, "r");
    ASSERT_NE(FAIL, vskey);
    absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

    ASSERT_EQ(SUCCEED, VSsetfields(vskey, "A,B"));

    std::vector<int32> read_buf(6, 0);
    EXPECT_EQ(3, VSread(vskey, reinterpret_cast<uint8*>(read_buf.data()), 3,
                        NO_INTERLACE));
    EXPECT_THAT(read_buf, ElementsAre(5, 6, 7, 500, 600, 700));
  }
}

TEST_F(VrwTest, MultiFieldCaseD_UserFullVdataNone) {
  const std::string path = GetTempFilePath("vrw_case_d.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  int32 vs_ref = -1;

  // Case D: user FULL_INTERLACE & vdata NO_INTERLACE.
  {
    const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ASSERT_EQ(SUCCEED, Vstart(fid));
    absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

    const int32 vskey = VSattach(fid, -1, "w");
    ASSERT_NE(FAIL, vskey);
    absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

    ASSERT_EQ(SUCCEED, VSsetinterlace(vskey, NO_INTERLACE));
    ASSERT_EQ(SUCCEED, VSfdefine(vskey, "F1", DFNT_INT16, 1));
    ASSERT_EQ(SUCCEED, VSfdefine(vskey, "F2", DFNT_INT16, 1));
    ASSERT_EQ(SUCCEED, VSsetfields(vskey, "F1,F2"));

    // User buffer FULL_INTERLACE: [f1_0, f2_0, f1_1, f2_1, ...]
    std::vector<int16> full_buf = {10, 100, 20, 200, 30, 300};
    EXPECT_EQ(3, VSwrite(vskey, reinterpret_cast<const uint8*>(full_buf.data()),
                         3, FULL_INTERLACE));
    vs_ref = VSQueryref(vskey);
  }

  {
    const int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ASSERT_EQ(SUCCEED, Vstart(fid));
    absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

    const int32 vskey = VSattach(fid, vs_ref, "r");
    ASSERT_NE(FAIL, vskey);
    absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

    ASSERT_EQ(SUCCEED, VSsetfields(vskey, "F1,F2"));

    std::vector<int16> read_buf(6, 0);
    EXPECT_EQ(3, VSread(vskey, reinterpret_cast<uint8*>(read_buf.data()), 3,
                        FULL_INTERLACE));
    EXPECT_THAT(read_buf, ElementsAre(10, 100, 20, 200, 30, 300));
  }
}

}  // namespace
}  // namespace hdf4
