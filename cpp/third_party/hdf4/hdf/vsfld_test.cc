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

// Tests for Vdata field definitions, queries, external files, and packing
// (third_party/hdf4/hdf/src/vsfld.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/vsfld.c

#include <cstdint>
#include <cstring>
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
#include "third_party/hdf4/hdf/src/hfile_priv.h"
#include "third_party/hdf4/hdf/src/hlimits.h"
#include "third_party/hdf4/hdf/src/vg.h"
#include "third_party/hdf4/hdf/src/vg_priv.h"

namespace hdf4 {
namespace {

using ::testing::ElementsAre;

std::string GetTempFilePath(const std::string& name) {
  return (std::filesystem::path(::testing::TempDir()) / name).string();
}

class VsfldTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }

  void TearDown() override {
    VSPshutdown();
    HEclear();
  }
};

TEST_F(VsfldTest, FieldDefineErrors) {
  const std::string path = GetTempFilePath("vsfld_define_errors.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

  // 1. Invalid key (not a VSID).
  HEclear();
  EXPECT_EQ(FAIL, VSfdefine(fid, "FIELD", DFNT_INT32, 1));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // 2. Closed/unallocated vkey.
  HEclear();
  EXPECT_EQ(FAIL, VSfdefine(999999, "FIELD", DFNT_INT32, 1));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  const int32 vskey = VSattach(fid, -1, "w");
  ASSERT_NE(FAIL, vskey);
  absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

  // 3. Null field name.
  HEclear();
  EXPECT_EQ(FAIL, VSfdefine(vskey, nullptr, DFNT_INT32, 1));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // 4. Empty field name.
  HEclear();
  EXPECT_EQ(FAIL, VSfdefine(vskey, "", DFNT_INT32, 1));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // 5. Multiple field names in one define call.
  HEclear();
  EXPECT_EQ(FAIL, VSfdefine(vskey, "F1,F2", DFNT_INT32, 1));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // 6. Bad order: order < 1.
  HEclear();
  EXPECT_EQ(FAIL, VSfdefine(vskey, "FIELD", DFNT_INT32, 0));
  EXPECT_EQ(DFE_BADORDER, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VSfdefine(vskey, "FIELD", DFNT_INT32, -5));
  EXPECT_EQ(DFE_BADORDER, HEvalue(1));

  // 7. Bad order: order > MAX_ORDER (65535).
  HEclear();
  EXPECT_EQ(FAIL, VSfdefine(vskey, "FIELD", DFNT_INT32, 65536));
  EXPECT_EQ(DFE_BADORDER, HEvalue(1));

  // 8. Invalid data type.
  HEclear();
  EXPECT_EQ(FAIL, VSfdefine(vskey, "FIELD", 999999, 1));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));

  // 9. Total size exceeds MAX_FIELD_SIZE (65535).
  HEclear();
  EXPECT_EQ(FAIL, VSfdefine(vskey, "FIELD", DFNT_INT32, 20000));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));
}

TEST_F(VsfldTest, FieldDefineValidAndRedefine) {
  const std::string path = GetTempFilePath("vsfld_define_valid.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

  const int32 vskey = VSattach(fid, -1, "w");
  ASSERT_NE(FAIL, vskey);
  absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

  // First definition allocates initial symbol table.
  EXPECT_EQ(SUCCEED, VSfdefine(vskey, "FIELD1", DFNT_INT32, 2));

  // Second definition reallocates symbol table.
  EXPECT_EQ(SUCCEED, VSfdefine(vskey, "FIELD2", DFNT_FLOAT32, 1));

  // Redefine FIELD1 where type and order differ from rstab[0] (PX:
  // DFNT_FLOAT32, 1). This triggers replacesym = 1.
  EXPECT_EQ(SUCCEED, VSfdefine(vskey, "FIELD1", DFNT_FLOAT64, 4));

  // Redefine FIELD2 where type matches rstab[0].type (DFNT_FLOAT32).
  // This leaves replacesym = 0 and appends duplicate definition.
  EXPECT_EQ(SUCCEED, VSfdefine(vskey, "FIELD2", DFNT_FLOAT32, 1));
}

TEST_F(VsfldTest, SetFieldsErrors) {
  const std::string path = GetTempFilePath("vsfld_setfields_errors.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

  // 1. Null fields pointer.
  HEclear();
  EXPECT_EQ(FAIL, VSsetfields(fid, nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // 2. Invalid key (not VSID).
  HEclear();
  EXPECT_EQ(FAIL, VSsetfields(fid, "PX"));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // 3. Unallocated vkey.
  HEclear();
  EXPECT_EQ(FAIL, VSsetfields(999999, "PX"));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  const int32 vskey = VSattach(fid, -1, "w");
  ASSERT_NE(FAIL, vskey);
  absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

  // 4. Empty fields string.
  HEclear();
  EXPECT_EQ(FAIL, VSsetfields(vskey, ""));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));

  // 5. Unknown field (not in user symbols and not in reserved table).
  HEclear();
  EXPECT_EQ(FAIL, VSsetfields(vskey, "NONEXISTENT_FIELD"));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));

  // 6. Record size exceeds MAX_FIELD_SIZE (65535).
  ASSERT_EQ(SUCCEED, VSfdefine(vskey, "BIG1", DFNT_INT32, 10000));
  ASSERT_EQ(SUCCEED, VSfdefine(vskey, "BIG2", DFNT_INT32, 10000));
  HEclear();
  EXPECT_EQ(FAIL, VSsetfields(vskey, "BIG1,BIG2"));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));
}

TEST_F(VsfldTest, SetFieldsWriteAndRead) {
  const std::string path = GetTempFilePath("vsfld_setfields_rw.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  // 1. Create file and define custom and reserved fields.
  {
    const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ASSERT_EQ(SUCCEED, Vstart(fid));
    absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

    const int32 vskey = VSattach(fid, -1, "w");
    ASSERT_NE(FAIL, vskey);
    absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

    ASSERT_EQ(SUCCEED, VSfdefine(vskey, "VAL", DFNT_INT32, 1));
    // Set fields combining reserved fields (PX, PY, PZ, IX, IY, IZ, NX, NY, NZ)
    // and custom (VAL).
    ASSERT_EQ(SUCCEED, VSsetfields(vskey, "PX,PY,VAL,IZ"));

    // Second call to VSsetfields when wlist->n > 0 on empty vdata returns FAIL.
    EXPECT_EQ(FAIL, VSsetfields(vskey, "PX,PY,VAL,IZ"));

    // Write a record.
    struct Record {
      float32 px;
      float32 py;
      int32_t val;
      int32_t iz;
    } rec = {1.5f, 2.5f, 42, 99};

    ASSERT_EQ(1, VSwrite(vskey, reinterpret_cast<const uint8*>(&rec), 1,
                         FULL_INTERLACE));
  }

  // 2. Re-open file and test setting fields for read.
  {
    const int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ASSERT_EQ(SUCCEED, Vstart(fid));
    absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

    const int32 vsref = VSgetid(fid, -1);
    ASSERT_GT(vsref, 0);

    const int32 vskey = VSattach(fid, vsref, "r");
    ASSERT_NE(FAIL, vskey);
    absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

    // VSsetexternalfile on read-only vdata fails with DFE_BADACC.
    HEclear();
    EXPECT_EQ(FAIL, VSsetexternalfile(vskey, "ext.dat", 0));
    EXPECT_EQ(DFE_BADACC, HEvalue(1));

    // Set fields for reading a subset.
    ASSERT_EQ(SUCCEED, VSsetfields(vskey, "VAL,IZ"));

    // Set fields again to exercise freeing previous rlist->item and
    // reallocating.
    ASSERT_EQ(SUCCEED, VSsetfields(vskey, "PX,VAL"));

    // Field not present in vdata read list.
    HEclear();
    EXPECT_EQ(FAIL, VSsetfields(vskey, "NONEXISTENT"));
    EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));
  }
}

TEST_F(VsfldTest, FieldQueries) {
  const std::string path = GetTempFilePath("vsfld_queries.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

  // 1. Queries with invalid vkey.
  HEclear();
  EXPECT_EQ(FAIL, VFnfields(fid));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(nullptr, VFfieldname(fid, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldtype(fid, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldisize(fid, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldesize(fid, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldorder(fid, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // 2. Queries with closed/unallocated vkey.
  HEclear();
  EXPECT_EQ(FAIL, VFnfields(999999));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(nullptr, VFfieldname(999999, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  const int32 vskey = VSattach(fid, -1, "w");
  ASSERT_NE(FAIL, vskey);
  absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

  // 3. Queries before fields are set (wlist.n == 0).
  EXPECT_EQ(0, VFnfields(vskey));

  HEclear();
  EXPECT_EQ(nullptr, VFfieldname(vskey, 0));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldtype(vskey, 0));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldisize(vskey, 0));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldesize(vskey, 0));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldorder(vskey, 0));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));

  // 4. Queries after setting fields.
  ASSERT_EQ(SUCCEED, VSfdefine(vskey, "TAGS", DFNT_UINT16, 3));
  ASSERT_EQ(SUCCEED, VSsetfields(vskey, "PX,TAGS"));

  EXPECT_EQ(2, VFnfields(vskey));

  // Field 0: PX (from rstab: DFNT_FLOAT32, order 1, size 4).
  EXPECT_STREQ("PX", VFfieldname(vskey, 0));
  EXPECT_EQ(DFNT_FLOAT32, VFfieldtype(vskey, 0));
  EXPECT_EQ(4, VFfieldisize(vskey, 0));
  EXPECT_EQ(4, VFfieldesize(vskey, 0));
  EXPECT_EQ(1, VFfieldorder(vskey, 0));

  // Field 1: TAGS (custom: DFNT_UINT16, order 3, size 6).
  EXPECT_STREQ("TAGS", VFfieldname(vskey, 1));
  EXPECT_EQ(DFNT_UINT16, VFfieldtype(vskey, 1));
  EXPECT_EQ(6, VFfieldisize(vskey, 1));
  EXPECT_EQ(6, VFfieldesize(vskey, 1));
  EXPECT_EQ(3, VFfieldorder(vskey, 1));
}

TEST_F(VsfldTest, ExternalFileOperations) {
  const std::string hdf_path = GetTempFilePath("vsfld_ext_main.hdf");
  const std::string ext_path = GetTempFilePath("vsfld_ext_sidecar.dat");
  std::filesystem::remove(hdf_path);
  std::filesystem::remove(ext_path);
  absl::Cleanup remove_files = [&] {
    std::filesystem::remove(hdf_path);
    std::filesystem::remove(ext_path);
  };

  const int32 fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

  const int32 vskey = VSattach(fid, -1, "w");
  ASSERT_NE(FAIL, vskey);
  absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

  ASSERT_EQ(SUCCEED, VSsetfields(vskey, "PX,PY"));

  // 1. VSsetexternalfile error checks.
  HEclear();
  EXPECT_EQ(FAIL, VSsetexternalfile(vskey, nullptr, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VSsetexternalfile(vskey, ext_path.c_str(), -1));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VSsetexternalfile(fid, ext_path.c_str(), 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // 2. VSgetexternalfile / info when element is not external (aid is normal
  // element).
  HEclear();
  char name_buf[128] = {};
  int32 offset = 0;
  int32 length = 0;
  EXPECT_EQ(FAIL,
            VSgetexternalfile(vskey, sizeof(name_buf), name_buf, &offset));
  EXPECT_EQ(DFE_INTERNAL, HEvalue(1));

  HEclear();
  EXPECT_EQ(0, VSgetexternalinfo(vskey, sizeof(name_buf), name_buf, &offset,
                                 &length));

  // 3. Valid VSsetexternalfile.
  ASSERT_EQ(SUCCEED, VSsetexternalfile(vskey, ext_path.c_str(), 16));

  // Call VSsetexternalfile again to exercise Hendaccess(vs->aid) replacement.
  ASSERT_EQ(SUCCEED, VSsetexternalfile(vskey, ext_path.c_str(), 32));

  // 4. Query external file info when external element exists.
  // Query length only (buf_size == 0).
  const int name_len = VSgetexternalfile(vskey, 0, nullptr, nullptr);
  EXPECT_EQ(ext_path.length(), name_len);

  const int info_len = VSgetexternalinfo(vskey, 0, nullptr, nullptr, nullptr);
  EXPECT_EQ(ext_path.length(), info_len);

  // Buffer provided but nullptr passed when buf_size != 0.
  HEclear();
  EXPECT_EQ(FAIL, VSgetexternalfile(vskey, 64, nullptr, nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VSgetexternalinfo(vskey, 64, nullptr, nullptr, nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // Truncated buffer size.
  char short_buf[6] = {};
  EXPECT_EQ(5, VSgetexternalfile(vskey, 5, short_buf, &offset));
  EXPECT_EQ(32, offset);

  EXPECT_EQ(5, VSgetexternalinfo(vskey, 5, short_buf, &offset, &length));
  EXPECT_EQ(32, offset);

  // Full buffer size.
  std::memset(name_buf, 0, sizeof(name_buf));
  offset = 0;
  EXPECT_EQ(ext_path.length(),
            VSgetexternalfile(vskey, sizeof(name_buf), name_buf, &offset));
  EXPECT_STREQ(ext_path.c_str(), name_buf);
  EXPECT_EQ(32, offset);

  std::memset(name_buf, 0, sizeof(name_buf));
  offset = 0;
  length = 0;
  EXPECT_EQ(ext_path.length(), VSgetexternalinfo(vskey, sizeof(name_buf),
                                                 name_buf, &offset, &length));
  EXPECT_STREQ(ext_path.c_str(), name_buf);
  EXPECT_EQ(32, offset);

  // Write data to external element.
  float32 coords[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  EXPECT_EQ(2, VSwrite(vskey, reinterpret_cast<const uint8*>(coords), 2,
                       FULL_INTERLACE));
}

TEST_F(VsfldTest, GetExternalInfoOnInternalVdata) {
  const std::string hdf_path = GetTempFilePath("vsfld_internal_vdata.hdf");
  std::filesystem::remove(hdf_path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(hdf_path); };

  const int32 fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

  const int32 vskey = VSattach(fid, -1, "w");
  ASSERT_NE(FAIL, vskey);
  absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

  ASSERT_EQ(SUCCEED, VSsetfields(vskey, "PX,PY"));

  float32 coords[2] = {10.0f, 20.0f};
  ASSERT_EQ(1, VSwrite(vskey, reinterpret_cast<const uint8*>(coords), 1,
                       FULL_INTERLACE));

  // The vdata has an aid, but it is stored as linked blocks (SPECIAL_LINKED),
  // not SPECIAL_EXT.
  char name_buf[64] = {};
  int32 offset = 0;
  int32 length = 0;

  // VSgetexternalfile returns FAIL for non-external elements.
  EXPECT_EQ(FAIL,
            VSgetexternalfile(vskey, sizeof(name_buf), name_buf, &offset));

  // VSgetexternalinfo returns 0 for non-external elements.
  EXPECT_EQ(0, VSgetexternalinfo(vskey, sizeof(name_buf), name_buf, &offset,
                                 &length));
}

TEST_F(VsfldTest, PackAndUnpackAllFields) {
  const std::string path = GetTempFilePath("vsfld_pack_all.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

  const int32 vskey = VSattach(fid, -1, "w");
  ASSERT_NE(FAIL, vskey);
  absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

  ASSERT_EQ(SUCCEED, VSfdefine(vskey, "TAG", DFNT_INT16, 1));
  ASSERT_EQ(SUCCEED, VSsetfields(vskey, "PX,TAG"));

  // Record structure: float32 PX (4 bytes) + int16 TAG (2 bytes) = 6 bytes.
  constexpr int kNumRecords = 3;
  constexpr int kRecSize = 6;
  constexpr int kBufSize = kRecSize * kNumRecords;
  uint8_t buffer[kBufSize] = {};

  std::vector<float32> px_in = {1.0f, 2.0f, 3.0f};
  std::vector<int16_t> tag_in = {10, 20, 30};
  void* fldptrs_in[2] = {px_in.data(), tag_in.data()};

  // 1. Pack all fields into buffer (fields_in_buf = nullptr, fields = nullptr).
  ASSERT_EQ(SUCCEED, VSfpack(vskey, _HDF_VSPACK, nullptr, buffer, kBufSize,
                             kNumRecords, nullptr, fldptrs_in));

  // Verify packed layout.
  for (int i = 0; i < kNumRecords; ++i) {
    float32 px_val = 0;
    int16_t tag_val = 0;
    std::memcpy(&px_val, buffer + i * kRecSize, sizeof(float32));
    std::memcpy(&tag_val, buffer + i * kRecSize + sizeof(float32),
                sizeof(int16_t));
    EXPECT_EQ(px_in[i], px_val);
    EXPECT_EQ(tag_in[i], tag_val);
  }

  // 2. Unpack all fields from buffer.
  std::vector<float32> px_out(kNumRecords, 0.0f);
  std::vector<int16_t> tag_out(kNumRecords, 0);
  void* fldptrs_out[2] = {px_out.data(), tag_out.data()};

  ASSERT_EQ(SUCCEED, VSfpack(vskey, _HDF_VSUNPACK, nullptr, buffer, kBufSize,
                             kNumRecords, nullptr, fldptrs_out));

  EXPECT_THAT(px_out, ElementsAre(1.0f, 2.0f, 3.0f));
  EXPECT_THAT(tag_out, ElementsAre(10, 20, 30));
}

TEST_F(VsfldTest, PackAndUnpackSubsetsAndErrors) {
  const std::string path = GetTempFilePath("vsfld_pack_subset.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

  const int32 vskey = VSattach(fid, -1, "w");
  ASSERT_NE(FAIL, vskey);
  absl::Cleanup detach_vs = [vskey] { EXPECT_EQ(SUCCEED, VSdetach(vskey)); };

  ASSERT_EQ(SUCCEED, VSfdefine(vskey, "ID", DFNT_INT32, 1));
  ASSERT_EQ(SUCCEED, VSfdefine(vskey, "VAL", DFNT_FLOAT32, 1));
  ASSERT_EQ(SUCCEED, VSsetfields(vskey, "PX,ID,VAL"));

  constexpr int kNumRecords = 2;
  constexpr int kRecSize = 12;  // PX(4) + ID(4) + VAL(4)
  constexpr int kBufSize = kRecSize * kNumRecords;
  uint8_t buffer[kBufSize] = {};

  std::vector<int32_t> id_in = {101, 102};
  void* fldptrs[1] = {id_in.data()};

  // 1. Pack with subset: buffer has "PX,ID,VAL", but fields to pack is only
  // "ID".
  ASSERT_EQ(SUCCEED, VSfpack(vskey, _HDF_VSPACK, "PX,ID,VAL", buffer, kBufSize,
                             kNumRecords, "ID", fldptrs));

  // Verify ID is at offset 4 in each 12-byte record.
  for (int i = 0; i < kNumRecords; ++i) {
    int32_t id_val = 0;
    std::memcpy(&id_val, buffer + i * kRecSize + 4, sizeof(int32_t));
    EXPECT_EQ(id_in[i], id_val);
  }

  // 2. Unpack only "ID" from buffer.
  std::vector<int32_t> id_out(kNumRecords, 0);
  void* fldptrs_out[1] = {id_out.data()};

  ASSERT_EQ(SUCCEED, VSfpack(vskey, _HDF_VSUNPACK, "PX,ID,VAL", buffer,
                             kBufSize, kNumRecords, "ID", fldptrs_out));
  EXPECT_THAT(id_out, ElementsAre(101, 102));

  // 3. Error checks.
  // Invalid vsid.
  HEclear();
  EXPECT_EQ(FAIL, VSfpack(fid, _HDF_VSPACK, nullptr, buffer, kBufSize,
                          kNumRecords, nullptr, fldptrs));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // Bad fields_in_buf name.
  HEclear();
  EXPECT_EQ(FAIL, VSfpack(vskey, _HDF_VSPACK, "UNKNOWN", buffer, kBufSize,
                          kNumRecords, nullptr, fldptrs));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));

  // Syntax error in fields_in_buf.
  HEclear();
  EXPECT_EQ(FAIL, VSfpack(vskey, _HDF_VSPACK, ",", buffer, kBufSize,
                          kNumRecords, nullptr, fldptrs));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));

  // Syntax error in fields.
  HEclear();
  EXPECT_EQ(FAIL, VSfpack(vskey, _HDF_VSPACK, "PX,ID", buffer, kBufSize,
                          kNumRecords, ",", fldptrs));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));

  // bufsz too small.
  HEclear();
  EXPECT_EQ(FAIL, VSfpack(vskey, _HDF_VSPACK, nullptr, buffer, 5, kNumRecords,
                          nullptr, fldptrs));
  EXPECT_EQ(DFE_NOTENOUGH, HEvalue(1));

  // fields not in fields_in_buf.
  HEclear();
  EXPECT_EQ(FAIL, VSfpack(vskey, _HDF_VSPACK, "PX,ID", buffer, kBufSize,
                          kNumRecords, "VAL", fldptrs));
  EXPECT_EQ(DFE_BADFIELDS, HEvalue(1));

  // Null pointer in fldbufpt.
  void* null_fldptrs[1] = {nullptr};
  HEclear();
  EXPECT_EQ(FAIL, VSfpack(vskey, _HDF_VSPACK, "PX,ID", buffer, kBufSize,
                          kNumRecords, "ID", null_fldptrs));
  EXPECT_EQ(DFE_BADPTR, HEvalue(1));

  // Null pointer in fldbufpt with fields == nullptr.
  void* null_fldptrs3[3] = {nullptr, nullptr, nullptr};
  HEclear();
  EXPECT_EQ(FAIL, VSfpack(vskey, _HDF_VSPACK, nullptr, buffer, kBufSize,
                          kNumRecords, nullptr, null_fldptrs3));
  EXPECT_EQ(DFE_BADPTR, HEvalue(1));
}

TEST_F(VsfldTest, DetachedKeyAndInternalFieldChecks) {
  const std::string path = GetTempFilePath("vsfld_detached.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup end_v = [fid] { EXPECT_EQ(SUCCEED, Vend(fid)); };

  const int32 vskey = VSattach(fid, -1, "w");
  ASSERT_NE(FAIL, vskey);

  // 1. Check queries when otag is temporarily corrupted.
  auto* w = static_cast<vsinstance_t*>(HAatom_object(vskey));
  ASSERT_NE(w, nullptr);
  ASSERT_NE(w->vs, nullptr);

  w->vs->otag = 0;
  HEclear();
  EXPECT_EQ(FAIL, VFnfields(vskey));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(nullptr, VFfieldname(vskey, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldtype(vskey, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldisize(vskey, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldesize(vskey, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldorder(vskey, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  w->vs->otag = VSDESCTAG;

  // 2. Checks on VSsetexternalfile internal validations:
  // vexistvs fails
  const uint16 saved_oref = w->vs->oref;
  w->vs->oref = 65500;
  HEclear();
  EXPECT_EQ(FAIL, VSsetexternalfile(vskey, "ext.dat", 0));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));
  w->vs->oref = saved_oref;

  // !w->ref
  const uint16 saved_ref = w->ref;
  w->ref = 0;
  HEclear();
  EXPECT_EQ(FAIL, VSsetexternalfile(vskey, "ext.dat", 0));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));
  w->ref = saved_ref;

  // 3. VSgetexternalinfo / getexternalfile with invalid aid:
  const int32 saved_aid = w->vs->aid;
  w->vs->aid = 999999;
  char name_buf[64] = {};
  int32 offset = 0;
  int32 length = 0;
  HEclear();
  EXPECT_EQ(FAIL,
            VSgetexternalfile(vskey, sizeof(name_buf), name_buf, &offset));
  EXPECT_EQ(DFE_INTERNAL, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VSgetexternalinfo(vskey, sizeof(name_buf), name_buf, &offset,
                                    &length));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  w->vs->aid = saved_aid;

  // Now detach vskey.
  ASSERT_EQ(SUCCEED, VSdetach(vskey));

  // 4. HAatom_object returns nullptr for detached vskey (in VSIDGROUP).
  HEclear();
  EXPECT_EQ(FAIL, VFnfields(vskey));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));

  HEclear();
  EXPECT_EQ(nullptr, VFfieldname(vskey, 0));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldtype(vskey, 0));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldisize(vskey, 0));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldesize(vskey, 0));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VFfieldorder(vskey, 0));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VSfdefine(vskey, "F", DFNT_INT32, 1));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VSsetfields(vskey, "PX"));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VSsetexternalfile(vskey, "ext.dat", 0));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL,
            VSgetexternalfile(vskey, sizeof(name_buf), name_buf, &offset));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, VSgetexternalinfo(vskey, sizeof(name_buf), name_buf, &offset,
                                    &length));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));

  uint8_t pack_buf[32] = {};
  void* pack_ptrs[1] = {pack_buf};
  HEclear();
  EXPECT_EQ(FAIL, VSfpack(vskey, _HDF_VSPACK, nullptr, pack_buf,
                          sizeof(pack_buf), 1, nullptr, pack_ptrs));
  EXPECT_EQ(DFE_NOVS, HEvalue(1));
}

}  // namespace
}  // namespace hdf4
