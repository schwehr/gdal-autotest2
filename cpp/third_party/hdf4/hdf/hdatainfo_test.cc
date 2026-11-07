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
#include <cstdio>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

#ifdef FAIL
#undef FAIL
#endif
#ifdef SUCCEED
#undef SUCCEED
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
// clang-format off
#include "third_party/hdf4/hdf/src/hdf_priv.h"
#include "third_party/hdf4/hdf/src/hcomp.h"
#include "third_party/hdf4/hdf/src/hcomp_priv.h"
#include "third_party/hdf4/hdf/src/hproto.h"
#include "third_party/hdf4/hdf/src/mfan.h"
#include "third_party/hdf4/hdf/src/mfan_priv.h"
#include "third_party/hdf4/hdf/src/mfgr.h"
#include "third_party/hdf4/hdf/src/mfgr_priv.h"
#include "third_party/hdf4/hdf/src/tbbt_priv.h"
#include "third_party/hdf4/hdf/src/vg.h"
#include "third_party/hdf4/hdf/src/vg_priv.h"
#include "third_party/hdf4/hdf/src/hdatainfo.h"
// clang-format on

namespace {

std::string TestFilePath(const std::string& name) {
  return (std::filesystem::path(::testing::TempDir()) / name).string();
}

uint16_t MakeSpecialTag(uint16_t tag) {
  return static_cast<uint16_t>((~tag & 0x8000) ? (tag | 0x4000) : DFTAG_NULL);
}

// -----------------------------------------------------------------------------
// HDgetdatainfo tests
// -----------------------------------------------------------------------------

TEST(HDataInfoTest, HDgetdatainfoArgValidation) {
  int32_t off = 0, len = 0;
  // info_count == 0 with non-null arrays
  EXPECT_EQ(FAIL, HDgetdatainfo(1, 100, 1, nullptr, 0, 0, &off, &len));

  // Only one array non-null
  EXPECT_EQ(FAIL, HDgetdatainfo(1, 100, 1, nullptr, 0, 1, &off, nullptr));
  EXPECT_EQ(FAIL, HDgetdatainfo(1, 100, 1, nullptr, 0, 1, nullptr, &len));

  // Invalid file id
  EXPECT_EQ(FAIL, HDgetdatainfo(-1, 100, 1, nullptr, 0, 1, &off, &len));
}

TEST(HDataInfoTest, HDgetdatainfoNonExistentElement) {
  const std::string path = TestFilePath("hdgetdatainfo_nonexistent.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  int32_t off = 0, len = 0;
  // Tag/ref that does not exist -> HTPselect fails -> DFE_CANTACCESS
  EXPECT_EQ(FAIL, HDgetdatainfo(fid, 9999, 9999, nullptr, 0, 1, &off, &len));
}

TEST(HDataInfoTest, HDgetdatainfoEmptyElement) {
  const std::string path = TestFilePath("hdgetdatainfo_empty.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  // Start access in write mode to create a DD entry with INVALID_OFFSET and
  // INVALID_LENGTH
  const uint16_t tag = 1000;
  const uint16_t ref = 1;
  const int32_t aid = Hstartaccess(fid, tag, ref, DFACC_WRITE);
  ASSERT_NE(FAIL, aid);
  EXPECT_NE(FAIL, Hendaccess(aid));

  int32_t off = 0, len = 0;
  // Should detect doff == INVALID_OFFSET && dlen == INVALID_LENGTH and return 0
  EXPECT_EQ(0, HDgetdatainfo(fid, tag, ref, nullptr, 0, 1, &off, &len));
  EXPECT_EQ(0, HDgetdatainfo(fid, tag, ref, nullptr, 0, 0, nullptr, nullptr));
}

TEST(HDataInfoTest, HDgetdatainfoContiguousElement) {
  const std::string path = TestFilePath("hdgetdatainfo_contig.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  const uint16_t tag = 1001;
  const uint16_t ref = 1;
  const char data[] = "Hello Contiguous World!";
  EXPECT_NE(FAIL,
            Hputelement(fid, tag, ref, reinterpret_cast<const uint8_t*>(data),
                        sizeof(data)));

  // start_block > 1 is invalid for contiguous element (only 1 block exists)
  int32_t off = 0, len = 0;
  EXPECT_EQ(FAIL, HDgetdatainfo(fid, tag, ref, nullptr, 2, 1, &off, &len));

  // Valid query with offset/length arrays
  EXPECT_EQ(1, HDgetdatainfo(fid, tag, ref, nullptr, 0, 1, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_EQ(sizeof(data), len);

  // Valid query without offset/length arrays (count only)
  EXPECT_EQ(1, HDgetdatainfo(fid, tag, ref, nullptr, 0, 0, nullptr, nullptr));
}

TEST(HDataInfoTest, HDgetdatainfoCompressedContiguous) {
  const std::string path = TestFilePath("hdgetdatainfo_comp.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  const uint16_t tag = 1100;
  const uint16_t ref = 1;
  model_info minfo;
  std::memset(&minfo, 0, sizeof(minfo));
  comp_info cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));

  const int32_t aid =
      HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &minfo, COMP_CODE_NONE, &cinfo);
  ASSERT_NE(FAIL, aid);
  const char payload[] =
      "Some uncompressed payload data for testing compression element";
  EXPECT_EQ(sizeof(payload), Hwrite(aid, sizeof(payload), payload));
  EXPECT_NE(FAIL, Hendaccess(aid));

  int32_t off = 0, len = 0;
  // start_block > 1 is invalid
  EXPECT_EQ(FAIL, HDgetdatainfo(fid, tag, ref, nullptr, 2, 1, &off, &len));

  // With arrays
  EXPECT_EQ(1, HDgetdatainfo(fid, tag, ref, nullptr, 0, 1, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_GT(len, 0);

  // Without arrays
  EXPECT_EQ(1, HDgetdatainfo(fid, tag, ref, nullptr, 0, 0, nullptr, nullptr));
}

TEST(HDataInfoTest, HDgetdatainfoCompressedEmpty) {
  const std::string path = TestFilePath("hdgetdatainfo_comp_empty.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  const uint16_t tag = 1101;
  const uint16_t ref = 1;
  model_info minfo;
  std::memset(&minfo, 0, sizeof(minfo));
  comp_info cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));

  // Create compressed element but write 0 bytes
  const int32_t aid =
      HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &minfo, COMP_CODE_NONE, &cinfo);
  ASSERT_NE(FAIL, aid);
  EXPECT_NE(FAIL, Hendaccess(aid));

  // length == 0 branch in HDgetdatainfo
  int32_t off = 0, len = 0;
  EXPECT_EQ(0, HDgetdatainfo(fid, tag, ref, nullptr, 0, 1, &off, &len));
  EXPECT_EQ(0, HDgetdatainfo(fid, tag, ref, nullptr, 0, 0, nullptr, nullptr));
}

TEST(HDataInfoTest, HDgetdatainfoCompressedLinkedBlocks) {
  const std::string path = TestFilePath("hdgetdatainfo_comp_linked.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  // 1. Create a linked-block element for DFTAG_COMPRESSED
  const uint16_t comp_ref = Hnewref(fid);
  const int32_t link_aid = HLcreate(fid, DFTAG_COMPRESSED, comp_ref, 16, 2);
  ASSERT_NE(FAIL, link_aid);
  const uint8_t payload[24] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                               13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
  EXPECT_EQ(sizeof(payload), Hwrite(link_aid, sizeof(payload), payload));
  EXPECT_NE(FAIL, Hendaccess(link_aid));

  // 2. Create special element pointing to DFTAG_COMPRESSED via SPECIAL_COMP
  // header
  const uint16_t tag = 1200;
  const uint16_t ref = Hnewref(fid);
  const uint16_t spec_tag = MakeSpecialTag(tag);

  uint8_t hdr[14] = {0};
  uint8_t* p = hdr;
  INT16ENCODE(p, SPECIAL_COMP);
  constexpr uint16_t kCompHeaderVersion = 0;
  UINT16ENCODE(p, kCompHeaderVersion);
  INT32ENCODE(p, sizeof(payload));  // uncompressed data length > 0
  UINT16ENCODE(p, comp_ref);

  const int32_t spec_aid = Hstartaccess(fid, spec_tag, ref, DFACC_ALL);
  ASSERT_NE(FAIL, spec_aid);
  EXPECT_EQ(sizeof(hdr), Hwrite(spec_aid, sizeof(hdr), hdr));
  EXPECT_NE(FAIL, Hendaccess(spec_aid));

  // Query datainfo: this tests spec_code == SPECIAL_LINKED inside SPECIAL_COMP!
  int32_t offsets[4] = {0}, lengths[4] = {0};
  const int count =
      HDgetdatainfo(fid, tag, ref, nullptr, 0, 4, offsets, lengths);
  EXPECT_GT(count, 0);

  // Without arrays
  const int count_no_arrays =
      HDgetdatainfo(fid, tag, ref, nullptr, 0, 0, nullptr, nullptr);
  EXPECT_EQ(count, count_no_arrays);
}

TEST(HDataInfoTest, HDgetdatainfoDirectLinkedBlocks) {
  const std::string path = TestFilePath("hdgetdatainfo_linked.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  const uint16_t tag = 1300;
  const uint16_t ref = Hnewref(fid);
  const int32_t link_aid = HLcreate(fid, tag, ref, 32, 4);
  ASSERT_NE(FAIL, link_aid);
  const uint8_t data[64] = {42};
  EXPECT_EQ(sizeof(data), Hwrite(link_aid, sizeof(data), data));
  EXPECT_NE(FAIL, Hendaccess(link_aid));

  int32_t offsets[4] = {0}, lengths[4] = {0};
  const int count =
      HDgetdatainfo(fid, tag, ref, nullptr, 0, 4, offsets, lengths);
  EXPECT_GT(count, 0);

  const int count_no_arrays =
      HDgetdatainfo(fid, tag, ref, nullptr, 0, 0, nullptr, nullptr);
  EXPECT_EQ(count, count_no_arrays);
}

TEST(HDataInfoTest, HDgetdatainfoChunked) {
  const std::string path = TestFilePath("hdgetdatainfo_chunked.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  const int32_t grid = GRstart(fid);
  ASSERT_NE(FAIL, grid);
  absl::Cleanup gr_closer = [grid] { GRend(grid); };

  const int32_t dims[2] = {10, 10};
  const int32_t riid =
      GRcreate(grid, "chunked_image", 1, DFNT_INT32, MFGR_INTERLACE_PIXEL,
               const_cast<int32_t*>(dims));
  ASSERT_NE(FAIL, riid);

  HDF_CHUNK_DEF cdef;
  cdef.chunk_lengths[0] = 5;
  cdef.chunk_lengths[1] = 5;
  EXPECT_NE(FAIL, GRsetchunk(riid, cdef, HDF_CHUNK));

  int32_t img_data[10][10] = {};
  int32_t start[2] = {0, 0};
  int32_t stride[2] = {1, 1};
  EXPECT_NE(FAIL, GRwriteimage(riid, start, stride, const_cast<int32_t*>(dims),
                               img_data));

  auto* ri_ptr = static_cast<ri_info_t*>(HAatom_object(riid));
  ASSERT_NE(nullptr, ri_ptr);
  const uint16_t tag = ri_ptr->img_tag;
  const uint16_t ref = ri_ptr->img_ref;

  // 1. Chunked element with chk_coord == NULL -> prints error and fails
  int32_t off = 0, len = 0;
  EXPECT_EQ(FAIL, HDgetdatainfo(fid, tag, ref, nullptr, 0, 1, &off, &len));

  // 2. Chunked element with valid chk_coord
  int32_t origin[2] = {0, 0};
  const int count = HDgetdatainfo(fid, tag, ref, origin, 0, 1, &off, &len);
  EXPECT_GT(count, 0);
  EXPECT_GT(off, 0);
  EXPECT_GT(len, 0);

  EXPECT_NE(FAIL, GRendaccess(riid));
}

// -----------------------------------------------------------------------------
// VSgetdatainfo tests
// -----------------------------------------------------------------------------

TEST(HDataInfoTest, VSgetdatainfoArgValidation) {
  int32_t off = 0, len = 0;
  // info_count == 0 with non-null arrays
  EXPECT_EQ(FAIL, VSgetdatainfo(1, 0, 0, &off, &len));

  // Only one array non-null
  EXPECT_EQ(FAIL, VSgetdatainfo(1, 0, 1, &off, nullptr));
  EXPECT_EQ(FAIL, VSgetdatainfo(1, 0, 1, nullptr, &len));

  // Invalid vsid (wrong atom group)
  EXPECT_EQ(FAIL, VSgetdatainfo(-1, 0, 1, &off, &len));
}

TEST(HDataInfoTest, VSgetdatainfoContiguousAndEmpty) {
  const std::string path = TestFilePath("vsgetdatainfo_contig.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  // 1. Empty vdata (no records written)
  const int32_t vsid_empty = VSattach(fid, -1, "w");
  ASSERT_NE(FAIL, vsid_empty);
  ASSERT_NE(FAIL, VSsetname(vsid_empty, "empty_vs"));
  ASSERT_NE(FAIL, VSfdefine(vsid_empty, "fld", DFNT_INT32, 1));
  ASSERT_NE(FAIL, VSsetfields(vsid_empty, "fld"));

  int32_t off = 0, len = 0;
  EXPECT_EQ(0, VSgetdatainfo(vsid_empty, 0, 1, &off, &len));
  EXPECT_EQ(0, VSgetdatainfo(vsid_empty, 0, 0, nullptr, nullptr));
  EXPECT_NE(FAIL, VSdetach(vsid_empty));

  // 2. Contiguous vdata with data written
  const int32_t data[5] = {1, 2, 3, 4, 5};
  const int32_t vs_ref =
      VHstoredata(fid, "fld", reinterpret_cast<const uint8_t*>(data), 5,
                  DFNT_INT32, "contig_vs", "class");
  ASSERT_NE(FAIL, vs_ref);

  const int32_t vsid = VSattach(fid, vs_ref, "r");
  ASSERT_NE(FAIL, vsid);

  EXPECT_EQ(1, VSgetdatainfo(vsid, 0, 1, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_EQ(5 * sizeof(int32_t), len);

  EXPECT_EQ(1, VSgetdatainfo(vsid, 0, 0, nullptr, nullptr));

  // Failure when Hoffset fails (line 413)
  auto* vs_inst = static_cast<vsinstance_t*>(HAatom_object(vsid));
  ASSERT_NE(nullptr, vs_inst);
  const int32_t orig_f = vs_inst->vs->f;
  vs_inst->vs->f = -1;
  EXPECT_EQ(FAIL, VSgetdatainfo(vsid, 0, 1, &off, &len));
  vs_inst->vs->f = orig_f;

  EXPECT_NE(FAIL, VSdetach(vsid));
}

TEST(HDataInfoTest, VSgetdatainfoLinkedBlocks) {
  const std::string path = TestFilePath("vsgetdatainfo_linked.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  const int32_t apvsid = VSattach(fid, -1, "w");
  ASSERT_NE(FAIL, apvsid);
  const int32_t vs1_ref = VSQueryref(apvsid);
  ASSERT_NE(FAIL, vs1_ref);

  ASSERT_NE(FAIL, VSfdefine(apvsid, "fld", DFNT_INT32, 1));
  ASSERT_NE(FAIL, VSsetfields(apvsid, "fld"));
  ASSERT_NE(FAIL, VSsetblocksize(apvsid, 32));
  ASSERT_NE(FAIL, VSsetnumblocks(apvsid, 4));

  const int32_t data1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  EXPECT_EQ(8, VSwrite(apvsid, reinterpret_cast<const uint8_t*>(data1), 8,
                       FULL_INTERLACE));

  // Write another vdata to prevent contiguous expansion
  const int32_t data_other[4] = {10, 20, 30, 40};
  EXPECT_NE(FAIL, VHstoredata(fid, "fld",
                              reinterpret_cast<const uint8_t*>(data_other), 4,
                              DFNT_INT32, "other_vs", "other_class"));

  // Write second batch of data to convert to linked blocks
  const int32_t data2[8] = {9, 10, 11, 12, 13, 14, 15, 16};
  EXPECT_EQ(8, VSwrite(apvsid, reinterpret_cast<const uint8_t*>(data2), 8,
                       FULL_INTERLACE));

  EXPECT_NE(FAIL, VSdetach(apvsid));

  // Re-attach to read linked blocks
  const int32_t vsid = VSattach(fid, vs1_ref, "w");
  ASSERT_NE(FAIL, vsid);

  const int n_blocks = VSgetdatainfo(vsid, 0, 0, nullptr, nullptr);
  EXPECT_GT(n_blocks, 1);

  // Failure when HDgetdatainfo fails (hits line 405)
  auto* vs_inst = static_cast<vsinstance_t*>(HAatom_object(vsid));
  ASSERT_NE(nullptr, vs_inst);
  const int32_t orig_vs_f = vs_inst->vs->f;
  vs_inst->vs->f = -1;
  int32_t bad_off = 0, bad_len = 0;
  EXPECT_EQ(FAIL, VSgetdatainfo(vsid, 0, 1, &bad_off, &bad_len));
  vs_inst->vs->f = orig_vs_f;

  std::vector<int32_t> offsets(n_blocks, 0);
  std::vector<int32_t> lengths(n_blocks, 0);
  EXPECT_EQ(n_blocks, VSgetdatainfo(vsid, 0, static_cast<unsigned>(n_blocks),
                                    offsets.data(), lengths.data()));
  for (int i = 0; i < n_blocks; ++i) {
    EXPECT_GT(offsets[i], 0);
    EXPECT_GT(lengths[i], 0);
  }

  EXPECT_NE(FAIL, VSdetach(vsid));
}

// -----------------------------------------------------------------------------
// Vgetattdatainfo tests
// -----------------------------------------------------------------------------

TEST(HDataInfoTest, VgetattdatainfoArgValidation) {
  int32_t off = 0, len = 0;
  // Null buffers
  EXPECT_EQ(FAIL, Vgetattdatainfo(1, 0, nullptr, &len));
  EXPECT_EQ(FAIL, Vgetattdatainfo(1, 0, &off, nullptr));

  // Invalid vgroup id group
  EXPECT_EQ(FAIL, Vgetattdatainfo(-1, 0, &off, &len));
}

TEST(HDataInfoTest, VgetattdatainfoNewAndOldAttributes) {
  const std::string path = TestFilePath("vgetattdatainfo.hdf");
  int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));

  const int32_t vgid = Vattach(fid, -1, "w");
  ASSERT_NE(FAIL, vgid);
  const int32_t vg_ref = VQueryref(vgid);
  ASSERT_NE(FAIL, vg_ref);

  // Negative attrindex
  int32_t off = 0, len = 0;
  EXPECT_EQ(FAIL, Vgetattdatainfo(vgid, -1, &off, &len));

  // No attrs yet -> out of range
  EXPECT_EQ(FAIL, Vgetattdatainfo(vgid, 0, &off, &len));

  // Add 1 new-style attribute
  const char new_attr_val[] = "new_attr_value";
  EXPECT_NE(FAIL, Vsetattr(vgid, "new_attr", DFNT_CHAR8, sizeof(new_attr_val),
                           new_attr_val));

  // Add 1 old-style attribute via VHstoredatam and Vaddtagref
  const char old_attr_val[] = "old_attr_value";
  const int32_t old_aref = VHstoredatam(
      fid, ATTR_FIELD_NAME, reinterpret_cast<const uint8_t*>(old_attr_val), 1,
      DFNT_CHAR8, "old_attr", _HDF_ATTRIBUTE, sizeof(old_attr_val));
  ASSERT_NE(FAIL, old_aref);
  EXPECT_NE(FAIL, Vaddtagref(vgid, DFTAG_VH, old_aref));

  EXPECT_NE(FAIL, Vdetach(vgid));
  EXPECT_NE(FAIL, Vend(fid));
  EXPECT_NE(FAIL, Hclose(fid));

  // Reopen file to load old-style attributes into vg->noldattrs
  fid = Hopen(path.c_str(), DFACC_RDWR, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };
  ASSERT_NE(FAIL, Vstart(fid));

  const int32_t vgid_read = Vattach(fid, vg_ref, "r");
  ASSERT_NE(FAIL, vgid_read);
  EXPECT_EQ(1, Vnoldattrs(vgid_read));

  // Index 0: old-style attribute
  EXPECT_EQ(1, Vgetattdatainfo(vgid_read, 0, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_GT(len, 0);

  // Index 1: new-style attribute
  EXPECT_EQ(1, Vgetattdatainfo(vgid_read, 1, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_GT(len, 0);

  // Index 2: out of range
  EXPECT_EQ(FAIL, Vgetattdatainfo(vgid_read, 2, &off, &len));

  // Test vg_alist == NULL (line 521)
  auto* vg_inst = static_cast<vginstance_t*>(HAatom_object(vgid_read));
  ASSERT_NE(nullptr, vg_inst);
  vg_attr_t* orig_alist = vg_inst->vg->alist;
  vg_inst->vg->alist = nullptr;
  EXPECT_EQ(FAIL, Vgetattdatainfo(vgid_read, 1, &off, &len));
  vg_inst->vg->alist = orig_alist;

  // Test VSattach failure (line 525)
  const uint16_t orig_aref = vg_inst->vg->alist[0].aref;
  vg_inst->vg->alist[0].aref = 9999;
  EXPECT_EQ(FAIL, Vgetattdatainfo(vgid_read, 1, &off, &len));
  vg_inst->vg->alist[0].aref = orig_aref;

  EXPECT_NE(FAIL, Vdetach(vgid_read));
}

// -----------------------------------------------------------------------------
// VSgetattdatainfo tests
// -----------------------------------------------------------------------------

TEST(HDataInfoTest, VSgetattdatainfoArgValidation) {
  int32_t off = 0, len = 0;
  // Null buffers
  EXPECT_EQ(FAIL, VSgetattdatainfo(1, 0, 0, nullptr, &len));
  EXPECT_EQ(FAIL, VSgetattdatainfo(1, 0, 0, &off, nullptr));

  // Invalid vsid
  EXPECT_EQ(FAIL, VSgetattdatainfo(-1, 0, 0, &off, &len));
}

TEST(HDataInfoTest, VSgetattdatainfoExecution) {
  const std::string path = TestFilePath("vsgetattdatainfo.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };
  ASSERT_NE(FAIL, Vstart(fid));

  const int32_t vsid = VSattach(fid, -1, "w");
  ASSERT_NE(FAIL, vsid);
  ASSERT_NE(FAIL, VSfdefine(vsid, "fld1", DFNT_INT32, 1));
  ASSERT_NE(FAIL, VSfdefine(vsid, "fld2", DFNT_INT32, 1));
  ASSERT_NE(FAIL, VSsetfields(vsid, "fld1,fld2"));

  int32_t off = 0, len = 0;
  // No attributes added yet -> nattrs == 0 -> FAIL
  EXPECT_EQ(FAIL, VSgetattdatainfo(vsid, _HDF_VDATA, 0, &off, &len));

  // Add attribute to vdata (_HDF_VDATA)
  const char vs_attr[] = "vs_attribute";
  EXPECT_NE(FAIL, VSsetattr(vsid, _HDF_VDATA, "vdata_attr", DFNT_CHAR8,
                            sizeof(vs_attr), vs_attr));

  // Add second attribute to vdata
  const char vs_attr2[] = "vs_attribute2";
  EXPECT_NE(FAIL, VSsetattr(vsid, _HDF_VDATA, "vdata_attr2", DFNT_CHAR8,
                            sizeof(vs_attr2), vs_attr2));

  // Add attribute to field 0
  const int32_t fld_val = 99;
  EXPECT_NE(FAIL, VSsetattr(vsid, 0, "field_attr", DFNT_INT32, 1, &fld_val));

  // findex out of range (< 0 and != _HDF_VDATA, or >= vs->wlist.n)
  EXPECT_EQ(FAIL, VSgetattdatainfo(vsid, -2, 0, &off, &len));
  EXPECT_EQ(FAIL, VSgetattdatainfo(vsid, 5, 0, &off, &len));

  // attrindex out of range (< 0 or >= nattrs)
  EXPECT_EQ(FAIL, VSgetattdatainfo(vsid, _HDF_VDATA, -1, &off, &len));
  EXPECT_EQ(FAIL, VSgetattdatainfo(vsid, _HDF_VDATA, 10, &off, &len));

  // Valid query for first vdata attribute
  EXPECT_EQ(1, VSgetattdatainfo(vsid, _HDF_VDATA, 0, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_GT(len, 0);

  // Valid query for second vdata attribute (hits vs_alist++)
  EXPECT_EQ(1, VSgetattdatainfo(vsid, _HDF_VDATA, 1, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_GT(len, 0);

  // Valid query for field attribute
  EXPECT_EQ(1, VSgetattdatainfo(vsid, 0, 0, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_GT(len, 0);

  // Field 1 exists in wlist, attrindex 0 < nattrs, but field 1 has no
  // attributes (hits line 629)
  EXPECT_EQ(FAIL, VSgetattdatainfo(vsid, 1, 0, &off, &len));

  // Search for second attribute on field 0 (only 1 exists, so !found)
  EXPECT_EQ(FAIL, VSgetattdatainfo(vsid, 0, 1, &off, &len));

  // Test vs_alist == NULL (line 613)
  auto* vs_inst = static_cast<vsinstance_t*>(HAatom_object(vsid));
  ASSERT_NE(nullptr, vs_inst);
  vs_attr_t* orig_vs_alist = vs_inst->vs->alist;
  vs_inst->vs->alist = nullptr;
  EXPECT_EQ(FAIL, VSgetattdatainfo(vsid, _HDF_VDATA, 0, &off, &len));
  vs_inst->vs->alist = orig_vs_alist;

  // Test VSattach failure (line 636)
  const uint16_t orig_vs_aref = vs_inst->vs->alist[0].aref;
  vs_inst->vs->alist[0].aref = 9999;
  EXPECT_EQ(FAIL, VSgetattdatainfo(vsid, _HDF_VDATA, 0, &off, &len));
  vs_inst->vs->alist[0].aref = orig_vs_aref;

  EXPECT_NE(FAIL, VSdetach(vsid));
}

// -----------------------------------------------------------------------------
// GRgetattdatainfo tests
// -----------------------------------------------------------------------------

TEST(HDataInfoTest, GRgetattdatainfoArgValidation) {
  int32_t off = 0, len = 0;
  // attrindex < 0
  EXPECT_EQ(FAIL, GRgetattdatainfo(1, -1, &off, &len));

  // Null buffers
  EXPECT_EQ(FAIL, GRgetattdatainfo(1, 0, nullptr, &len));
  EXPECT_EQ(FAIL, GRgetattdatainfo(1, 0, &off, nullptr));

  // Invalid id group
  EXPECT_EQ(FAIL, GRgetattdatainfo(-1, 0, &off, &len));
}

TEST(HDataInfoTest, GRgetattdatainfoFileAndImageAttributes) {
  const std::string path = TestFilePath("grgetattdatainfo.hdf");
  int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));

  int32_t grid = GRstart(fid);
  ASSERT_NE(FAIL, grid);

  // Add file attributes
  const char file_attr[] = "file_attribute_data";
  EXPECT_NE(FAIL, GRsetattr(grid, "file_attr", DFNT_CHAR8, sizeof(file_attr),
                            file_attr));
  const char file_attr2[] = "file_attribute_2";
  EXPECT_NE(FAIL, GRsetattr(grid, "file_attr2", DFNT_CHAR8, sizeof(file_attr2),
                            file_attr2));

  // Create image and add image attributes
  const int32_t dims[2] = {4, 4};
  const int32_t riid =
      GRcreate(grid, "img", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL,
               const_cast<int32_t*>(dims));
  ASSERT_NE(FAIL, riid);

  const int32_t img_attr[2] = {123, 456};
  EXPECT_NE(FAIL, GRsetattr(riid, "img_attr", DFNT_INT32, 2, img_attr));
  const int32_t img_attr2[2] = {789, 1011};
  EXPECT_NE(FAIL, GRsetattr(riid, "img_attr2", DFNT_INT32, 2, img_attr2));

  EXPECT_NE(FAIL, GRendaccess(riid));
  EXPECT_NE(FAIL, GRend(grid));
  EXPECT_NE(FAIL, Vend(fid));
  EXPECT_NE(FAIL, Hclose(fid));

  // Reopen file to read persisted attributes
  fid = Hopen(path.c_str(), DFACC_RDWR, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  grid = GRstart(fid);
  ASSERT_NE(FAIL, grid);
  absl::Cleanup gr_closer = [grid] { GRend(grid); };

  int32_t off = 0, len = 0;
  // attrindex >= gattr_count
  EXPECT_EQ(FAIL, GRgetattdatainfo(grid, 5, &off, &len));

  // Valid file attributes
  EXPECT_EQ(1, GRgetattdatainfo(grid, 0, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_GT(len, 0);

  // File attribute index 1 (hits tbbtnext, line 757)
  EXPECT_EQ(1, GRgetattdatainfo(grid, 1, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_GT(len, 0);

  // Test VSattach failure for file attribute (line 765)
  auto* gr_ptr = static_cast<gr_info_t*>(HAatom_object(grid));
  ASSERT_NE(nullptr, gr_ptr);
  auto* node = static_cast<TBBT_NODE*>(tbbtfirst(gr_ptr->gattree->root));
  ASSERT_NE(nullptr, node);
  auto* at_ptr = static_cast<at_info_t*>(node->data);
  ASSERT_NE(nullptr, at_ptr);
  const uint16_t orig_ref = at_ptr->ref;
  at_ptr->ref = 9999;
  EXPECT_EQ(FAIL, GRgetattdatainfo(grid, 0, &off, &len));
  at_ptr->ref = orig_ref;

  const int32_t riid_read = GRselect(grid, 0);
  ASSERT_NE(FAIL, riid_read);

  // attrindex >= lattr_count
  EXPECT_EQ(FAIL, GRgetattdatainfo(riid_read, 3, &off, &len));

  // Valid image attributes
  EXPECT_EQ(1, GRgetattdatainfo(riid_read, 0, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_GT(len, 0);

  // Image attribute index 1 (hits tbbtnext, line 757)
  EXPECT_EQ(1, GRgetattdatainfo(riid_read, 1, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_GT(len, 0);

  EXPECT_NE(FAIL, GRendaccess(riid_read));
}

// -----------------------------------------------------------------------------
// GRgetdatainfo tests
// -----------------------------------------------------------------------------

TEST(HDataInfoTest, GRgetdatainfoArgValidation) {
  int32_t off = 0, len = 0;
  // info_count == 0 with non-null arrays
  EXPECT_EQ(FAIL, GRgetdatainfo(1, 0, 0, &off, &len));

  // Only one array non-null
  EXPECT_EQ(FAIL, GRgetdatainfo(1, 0, 1, &off, nullptr));
  EXPECT_EQ(FAIL, GRgetdatainfo(1, 0, 1, nullptr, &len));

  // Invalid riid group
  EXPECT_EQ(FAIL, GRgetdatainfo(-1, 0, 1, &off, &len));
}

TEST(HDataInfoTest, GRgetdatainfoExecution) {
  const std::string path = TestFilePath("grgetdatainfo.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  const int32_t grid = GRstart(fid);
  ASSERT_NE(FAIL, grid);
  absl::Cleanup gr_closer = [grid] { GRend(grid); };

  // 1. Newly created image without data
  const int32_t dims[2] = {4, 4};
  const int32_t riid_nodata =
      GRcreate(grid, "no_data_img", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL,
               const_cast<int32_t*>(dims));
  ASSERT_NE(FAIL, riid_nodata);

  int32_t off = 999, len = 999;
  EXPECT_EQ(0, GRgetdatainfo(riid_nodata, 0, 1, &off, &len));
  EXPECT_EQ(0, off);
  EXPECT_EQ(0, len);
  EXPECT_EQ(0, GRgetdatainfo(riid_nodata, 0, 0, nullptr, nullptr));
  EXPECT_NE(FAIL, GRendaccess(riid_nodata));

  // 2. Image with data written
  const int32_t riid =
      GRcreate(grid, "img_with_data", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL,
               const_cast<int32_t*>(dims));
  ASSERT_NE(FAIL, riid);
  const uint8_t pixels[16] = {1, 2,  3,  4,  5,  6,  7,  8,
                              9, 10, 11, 12, 13, 14, 15, 16};
  int32_t start[2] = {0, 0};
  int32_t stride[2] = {1, 1};
  EXPECT_NE(FAIL, GRwriteimage(riid, start, stride, const_cast<int32_t*>(dims),
                               const_cast<uint8_t*>(pixels)));

  EXPECT_EQ(1, GRgetdatainfo(riid, 0, 1, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_EQ(sizeof(pixels), len);

  EXPECT_EQ(1, GRgetdatainfo(riid, 0, 0, nullptr, nullptr));

  // start_block > 1 on contiguous image causes HDgetdatainfo to fail (lines
  // 862, 870)
  EXPECT_EQ(FAIL, GRgetdatainfo(riid, 5, 0, nullptr, nullptr));
  EXPECT_EQ(FAIL, GRgetdatainfo(riid, 5, 1, &off, &len));

  // 3. Image with tag/ref set to non-existent data -> Hlength returns FAIL
  // (lines 851-855)
  auto* ri_ptr = static_cast<ri_info_t*>(HAatom_object(riid));
  ASSERT_NE(nullptr, ri_ptr);
  const uint16_t orig_img_ref = ri_ptr->img_ref;
  ri_ptr->img_ref = 9999;
  off = 999;
  len = 999;
  EXPECT_EQ(0, GRgetdatainfo(riid, 0, 1, &off, &len));
  EXPECT_EQ(0, off);
  EXPECT_EQ(0, len);
  ri_ptr->img_ref = orig_img_ref;

  EXPECT_NE(FAIL, GRendaccess(riid));
}

// -----------------------------------------------------------------------------
// GRgetpalinfo tests
// -----------------------------------------------------------------------------

TEST(HDataInfoTest, GRgetpalinfoArgValidation) {
  // Invalid grid group
  EXPECT_EQ(FAIL, GRgetpalinfo(-1, 0, nullptr));

  // pal_count > 0 with null array
  EXPECT_EQ(FAIL, GRgetpalinfo(1, 5, nullptr));
}

TEST(HDataInfoTest, GRgetpalinfoExecution) {
  const std::string path = TestFilePath("grgetpalinfo.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  const int32_t grid = GRstart(fid);
  ASSERT_NE(FAIL, grid);
  absl::Cleanup gr_closer = [grid] { GRend(grid); };

  // pal_count > 0 with null array (line 930)
  EXPECT_EQ(FAIL, GRgetpalinfo(grid, 5, nullptr));

  // When no palettes exist: pal_count == 0 && palinfo_array == NULL returns 0
  EXPECT_EQ(0, GRgetpalinfo(grid, 0, nullptr));

  // When Hnumber fails (lines 939-940)
  auto* gr_ptr = static_cast<gr_info_t*>(HAatom_object(grid));
  ASSERT_NE(nullptr, gr_ptr);
  const int32_t orig_hfid = gr_ptr->hdf_file_id;
  gr_ptr->hdf_file_id = -1;
  EXPECT_EQ(FAIL, GRgetpalinfo(grid, 0, nullptr));
  gr_ptr->hdf_file_id = orig_hfid;

  // Add 1 DFTAG_IP8 and 1 DFTAG_LUT palette directly into file
  const uint8_t palette[768] = {1, 2, 3};
  EXPECT_NE(FAIL, Hputelement(fid, DFTAG_IP8, Hnewref(fid), palette,
                              sizeof(palette)));
  EXPECT_NE(FAIL, Hputelement(fid, DFTAG_LUT, Hnewref(fid), palette,
                              sizeof(palette)));

  // Total count should now be 2
  EXPECT_EQ(2, GRgetpalinfo(grid, 0, nullptr));

  // Retrieve palette info array
  hdf_ddinfo_t dinfo[2];
  const int count = GRgetpalinfo(grid, 2, dinfo);
  EXPECT_EQ(2, count);
  EXPECT_TRUE((dinfo[0].tag == DFTAG_IP8 && dinfo[1].tag == DFTAG_LUT) ||
              (dinfo[0].tag == DFTAG_LUT && dinfo[1].tag == DFTAG_IP8));
  EXPECT_EQ(sizeof(palette), dinfo[0].length);
  EXPECT_EQ(sizeof(palette), dinfo[1].length);
}

// -----------------------------------------------------------------------------
// ANgetdatainfo tests
// -----------------------------------------------------------------------------

TEST(HDataInfoTest, ANgetdatainfoArgValidation) {
  int32_t off = 0, len = 0;
  // Null buffers
  EXPECT_EQ(FAIL, ANgetdatainfo(1, nullptr, &len));
  EXPECT_EQ(FAIL, ANgetdatainfo(1, &off, nullptr));

  // Invalid ann_id
  EXPECT_EQ(FAIL, ANgetdatainfo(-1, &off, &len));
}

TEST(HDataInfoTest, ANgetdatainfoExecution) {
  const std::string path = TestFilePath("angetdatainfo.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  absl::Cleanup closer = [fid, &path] {
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  const int32_t an_id = ANstart(fid);
  ASSERT_NE(FAIL, an_id);
  absl::Cleanup an_closer = [an_id] { ANend(an_id); };

  int32_t off = 0, len = 0;

  // 1. AN_DATA_LABEL (adjusts offset +4, length -4)
  const int32_t ann_dl = ANcreate(an_id, 1000, 1, AN_DATA_LABEL);
  ASSERT_NE(FAIL, ann_dl);
  // Before ANwriteann: new_ann != 0, returns SUCCEED without modification
  EXPECT_EQ(SUCCEED, ANgetdatainfo(ann_dl, &off, &len));
  const char dl_text[] = "DataLabelText";
  EXPECT_NE(FAIL, ANwriteann(ann_dl, dl_text, sizeof(dl_text)));
  EXPECT_EQ(SUCCEED, ANgetdatainfo(ann_dl, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_EQ(sizeof(dl_text), len);

  // Corrupt annotation key to bad type (lines 1063-1064)
  auto* ann_node = static_cast<ANnode*>(HAatom_object(ann_dl));
  ASSERT_NE(nullptr, ann_node);
  const uint32_t orig_key = ann_node->ann_key;
  ann_node->ann_key = (static_cast<uint32_t>(99) << 16) | 1;
  EXPECT_EQ(FAIL, ANgetdatainfo(ann_dl, &off, &len));
  ann_node->ann_key = orig_key;

  // Corrupt file_id to bad file rec (line 1041)
  const int32_t orig_fid = ann_node->file_id;
  ann_node->file_id = -1;
  EXPECT_EQ(FAIL, ANgetdatainfo(ann_dl, &off, &len));
  ann_node->file_id = orig_fid;

  // Corrupt ref to non-existent on disk (lines 1084-1085)
  ann_node->ann_key = (static_cast<uint32_t>(AN_DATA_LABEL) << 16) | 9999;
  EXPECT_EQ(FAIL, ANgetdatainfo(ann_dl, &off, &len));
  ann_node->ann_key = orig_key;

  EXPECT_NE(FAIL, ANendaccess(ann_dl));

  // 2. AN_DATA_DESC (adjusts offset +4, length -4)
  const int32_t ann_dd = ANcreate(an_id, 1000, 1, AN_DATA_DESC);
  ASSERT_NE(FAIL, ann_dd);
  const char dd_text[] = "DataDescTextDescription";
  EXPECT_NE(FAIL, ANwriteann(ann_dd, dd_text, sizeof(dd_text)));
  EXPECT_EQ(SUCCEED, ANgetdatainfo(ann_dd, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_EQ(sizeof(dd_text), len);
  EXPECT_NE(FAIL, ANendaccess(ann_dd));

  // 3. AN_FILE_LABEL (DFTAG_FID, no 4-byte offset adjustment)
  const int32_t ann_fl = ANcreatef(an_id, AN_FILE_LABEL);
  ASSERT_NE(FAIL, ann_fl);
  const char fl_text[] = "FileLabelText";
  EXPECT_NE(FAIL, ANwriteann(ann_fl, fl_text, sizeof(fl_text)));
  EXPECT_EQ(SUCCEED, ANgetdatainfo(ann_fl, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_EQ(sizeof(fl_text), len);
  EXPECT_NE(FAIL, ANendaccess(ann_fl));

  // 4. AN_FILE_DESC (DFTAG_FD, no 4-byte offset adjustment)
  const int32_t ann_fd = ANcreatef(an_id, AN_FILE_DESC);
  ASSERT_NE(FAIL, ann_fd);
  const char fd_text[] = "FileDescText";
  EXPECT_NE(FAIL, ANwriteann(ann_fd, fd_text, sizeof(fd_text)));
  EXPECT_EQ(SUCCEED, ANgetdatainfo(ann_fd, &off, &len));
  EXPECT_GT(off, 0);
  EXPECT_EQ(sizeof(fd_text), len);
  EXPECT_NE(FAIL, ANendaccess(ann_fd));
}

TEST(HDataInfoTest, InvalidAtomObjects) {
  const std::string path = TestFilePath("invalid_atoms.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Vstart(fid));
  const int32_t grid = GRstart(fid);
  ASSERT_NE(FAIL, grid);
  absl::Cleanup closer = [fid, grid, &path] {
    GRend(grid);
    Vend(fid);
    Hclose(fid);
    std::remove(path.c_str());
  };

  int32_t off = 0, len = 0;

  // VSIDGROUP with null atom object
  int32_t fake_vsid = HAregister_atom(VSIDGROUP, nullptr);
  ASSERT_NE(FAIL, fake_vsid);
  EXPECT_EQ(FAIL, VSgetdatainfo(fake_vsid, 0, 1, &off, &len));
  EXPECT_EQ(FAIL, VSgetattdatainfo(fake_vsid, _HDF_VDATA, 0, &off, &len));
  EXPECT_EQ(nullptr, HAremove_atom(fake_vsid));

  // VSIDGROUP with vs == nullptr
  vsinstance_t fake_vs_inst = {};
  fake_vs_inst.vs = nullptr;
  fake_vsid = HAregister_atom(VSIDGROUP, &fake_vs_inst);
  ASSERT_NE(FAIL, fake_vsid);
  EXPECT_EQ(FAIL, VSgetdatainfo(fake_vsid, 0, 1, &off, &len));
  EXPECT_EQ(FAIL, VSgetattdatainfo(fake_vsid, _HDF_VDATA, 0, &off, &len));
  EXPECT_EQ(&fake_vs_inst, HAremove_atom(fake_vsid));

  // VSIDGROUP with invalid aid in vs
  VDATA fake_vs = {};
  fake_vs.aid = FAIL;
  fake_vs_inst.vs = &fake_vs;
  fake_vsid = HAregister_atom(VSIDGROUP, &fake_vs_inst);
  ASSERT_NE(FAIL, fake_vsid);
  EXPECT_EQ(FAIL, VSgetdatainfo(fake_vsid, 0, 1, &off, &len));
  EXPECT_EQ(&fake_vs_inst, HAremove_atom(fake_vsid));

  // VGIDGROUP with null atom object
  int32_t fake_vgid = HAregister_atom(VGIDGROUP, nullptr);
  ASSERT_NE(FAIL, fake_vgid);
  EXPECT_EQ(FAIL, Vgetattdatainfo(fake_vgid, 0, &off, &len));
  EXPECT_EQ(nullptr, HAremove_atom(fake_vgid));

  // VGIDGROUP with vg == nullptr
  vginstance_t fake_vg_inst = {};
  fake_vg_inst.vg = nullptr;
  fake_vgid = HAregister_atom(VGIDGROUP, &fake_vg_inst);
  ASSERT_NE(FAIL, fake_vgid);
  EXPECT_EQ(FAIL, Vgetattdatainfo(fake_vgid, 0, &off, &len));
  EXPECT_EQ(&fake_vg_inst, HAremove_atom(fake_vgid));

  // GRIDGROUP with null atom object
  int32_t fake_grid = HAregister_atom(GRIDGROUP, nullptr);
  ASSERT_NE(FAIL, fake_grid);
  EXPECT_EQ(FAIL, GRgetattdatainfo(fake_grid, 0, &off, &len));
  EXPECT_EQ(FAIL, GRgetpalinfo(fake_grid, 0, nullptr));
  EXPECT_EQ(nullptr, HAremove_atom(fake_grid));

  // RIIDGROUP with null atom object
  int32_t fake_riid = HAregister_atom(RIIDGROUP, nullptr);
  ASSERT_NE(FAIL, fake_riid);
  EXPECT_EQ(FAIL, GRgetattdatainfo(fake_riid, 0, &off, &len));
  EXPECT_EQ(FAIL, GRgetdatainfo(fake_riid, 0, 1, &off, &len));
  EXPECT_EQ(nullptr, HAremove_atom(fake_riid));
}

}  // namespace
