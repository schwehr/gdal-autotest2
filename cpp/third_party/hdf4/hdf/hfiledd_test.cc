// Copyright 2024 The HDF4 Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/hfile.h"
#include "third_party/hdf4/hdf/src/hfile_priv.h"

namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::Le;
using ::testing::Lt;
using ::testing::Ne;
using ::testing::NotNull;

void EncodeDD(uint8_t*& p, uint16_t tag, uint16_t ref, int32_t offset,
              int32_t length) {
  UINT16ENCODE(p, tag);
  UINT16ENCODE(p, ref);
  INT32ENCODE(p, offset);
  INT32ENCODE(p, length);
}

std::string GetTempFilePath(std::string_view name) {
  return absl::StrCat(testing::TempDir(), "/", name);
}

TEST(HfileddTest, TagCompareAndDestroyNode) {
  uint16 k1 = 100;
  uint16 k2 = 200;
  uint16 k3 = 100;

  EXPECT_THAT(tagcompare(&k1, &k2, 0), Lt(0));
  EXPECT_THAT(tagcompare(&k2, &k1, 0), Gt(0));
  EXPECT_THAT(tagcompare(&k1, &k3, 0), Eq(0));

  // tagdestroynode with populated bit-vector and dynarray
  tag_info* t1 = static_cast<tag_info*>(malloc(sizeof(tag_info)));
  ASSERT_THAT(t1, NotNull());
  t1->tag = 100;
  t1->b = bv_new(-1);
  t1->d = DAcreate_array(10, 10);
  tagdestroynode(t1);

  // tagdestroynode with null bit-vector and dynarray
  tag_info* t2 = static_cast<tag_info*>(malloc(sizeof(tag_info)));
  ASSERT_THAT(t2, NotNull());
  t2->tag = 101;
  t2->b = nullptr;
  t2->d = nullptr;
  tagdestroynode(t2);
}

TEST(HfileddTest, TagRefGenerationAndCounting) {
  const std::string path = GetTempFilePath("hfiledd_tag_ref.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Error case for file id
  EXPECT_THAT(Hnumber(FAIL, DFTAG_WILDCARD), Eq(FAIL));
  EXPECT_THAT(Hnewref(FAIL), Eq(0));
  EXPECT_THAT(Htagnewref(FAIL, 100), Eq(0));

  // Initial count with wildcard (contains version element)
  const int32_t initial_cnt = Hnumber(fid, DFTAG_WILDCARD);
  EXPECT_THAT(initial_cnt, Ge(1));

  // Non-existent tag count
  EXPECT_THAT(Hnumber(fid, 9999), Eq(0));

  // Test Hnewref
  const uint16 r1 = Hnewref(fid);
  const uint16 r2 = Hnewref(fid);
  EXPECT_THAT(r1, Gt(0));
  EXPECT_THAT(r2, Gt(r1));

  // Test Htagnewref on unused tag
  const uint16 tr1 = Htagnewref(fid, 500);
  EXPECT_THAT(tr1, Eq(1));

  // Write elements with tag 500
  const int32_t aid1 = Hstartwrite(fid, 500, tr1, 4);
  ASSERT_THAT(aid1, Ne(FAIL));
  uint8_t d[4] = {1, 2, 3, 4};
  EXPECT_THAT(Hwrite(aid1, 4, d), Eq(4));
  EXPECT_THAT(Hendaccess(aid1), Eq(SUCCEED));

  // Now Htagnewref should return 2
  const uint16 tr2 = Htagnewref(fid, 500);
  EXPECT_THAT(tr2, Eq(2));

  const int32_t aid2 = Hstartwrite(fid, 500, tr2, 4);
  ASSERT_THAT(aid2, Ne(FAIL));
  EXPECT_THAT(Hwrite(aid2, 4, d), Eq(4));
  EXPECT_THAT(Hendaccess(aid2), Eq(SUCCEED));

  // Number of tag 500 elements is now 2
  EXPECT_THAT(Hnumber(fid, 500), Eq(2));
  EXPECT_THAT(Hnumber(fid, DFTAG_WILDCARD), Eq(initial_cnt + 2));

  // Count DFTAG_NULL and DFTAG_FREE
  EXPECT_THAT(Hnumber(fid, DFTAG_NULL), Ge(0));
  EXPECT_THAT(Hnumber(fid, DFTAG_FREE), Ge(0));

  // Test Hnewref when maxref reaches MAX_REF
  filerec_t* frec = static_cast<filerec_t*>(HAatom_object(fid));
  ASSERT_THAT(frec, NotNull());
  const uint16 orig_maxref = frec->maxref;
  frec->maxref = MAX_REF;
  const uint16 wrapped_ref = Hnewref(fid);
  EXPECT_THAT(wrapped_ref, Gt(0));
  EXPECT_THAT(wrapped_ref, Le(MAX_REF));
  frec->maxref = orig_maxref;
}

TEST(HfileddTest, FindElementsForwardAndBackward) {
  const std::string path = GetTempFilePath("hfiledd_find.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Write elements with tag 600 refs 1, 2, 3 and tag 601 ref 1
  uint8_t data[4] = {10, 20, 30, 40};
  for (uint16 ref = 1; ref <= 3; ++ref) {
    const int32_t aid = Hstartwrite(fid, 600, ref, 4);
    ASSERT_THAT(aid, Ne(FAIL));
    EXPECT_THAT(Hwrite(aid, 4, data), Eq(4));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  }
  const int32_t aid_other = Hstartwrite(fid, 601, 1, 4);
  ASSERT_THAT(aid_other, Ne(FAIL));
  EXPECT_THAT(Hwrite(aid_other, 4, data), Eq(4));
  EXPECT_THAT(Hendaccess(aid_other), Eq(SUCCEED));

  // Error cases for Hfind
  uint16 tag = 0;
  uint16 ref = 0;
  int32_t off = 0;
  int32_t len = 0;
  EXPECT_THAT(Hfind(FAIL, DFTAG_WILDCARD, DFREF_WILDCARD, &tag, &ref, &off,
                    &len, DF_FORWARD),
              Eq(FAIL));
  EXPECT_THAT(Hfind(fid, DFTAG_WILDCARD, DFREF_WILDCARD, nullptr, &ref, &off,
                    &len, DF_FORWARD),
              Eq(FAIL));
  EXPECT_THAT(Hfind(fid, DFTAG_WILDCARD, DFREF_WILDCARD, &tag, nullptr, &off,
                    &len, DF_FORWARD),
              Eq(FAIL));
  EXPECT_THAT(Hfind(fid, DFTAG_WILDCARD, DFREF_WILDCARD, &tag, &ref, nullptr,
                    &len, DF_FORWARD),
              Eq(FAIL));
  EXPECT_THAT(Hfind(fid, DFTAG_WILDCARD, DFREF_WILDCARD, &tag, &ref, &off,
                    nullptr, DF_FORWARD),
              Eq(FAIL));
  EXPECT_THAT(
      Hfind(fid, DFTAG_WILDCARD, DFREF_WILDCARD, &tag, &ref, &off, &len, 999),
      Eq(FAIL));

  // Forward search all elements
  tag = 0;
  ref = 0;
  int count_fwd = 0;
  while (Hfind(fid, DFTAG_WILDCARD, DFREF_WILDCARD, &tag, &ref, &off, &len,
               DF_FORWARD) == SUCCEED) {
    count_fwd++;
    EXPECT_THAT(tag, Gt(0));
    EXPECT_THAT(ref, Gt(0));
  }
  EXPECT_THAT(count_fwd, Ge(5));  // 1 version + 3 tag 600 + 1 tag 601

  // Backward search all elements
  tag = 0;
  ref = 0;
  int count_bwd = 0;
  while (Hfind(fid, DFTAG_WILDCARD, DFREF_WILDCARD, &tag, &ref, &off, &len,
               DF_BACKWARD) == SUCCEED) {
    count_bwd++;
    EXPECT_THAT(tag, Gt(0));
    EXPECT_THAT(ref, Gt(0));
  }
  EXPECT_THAT(count_bwd, Eq(count_fwd));

  // Search specific tag with wildcard ref (forward)
  tag = 0;
  ref = 0;
  int count_tag600 = 0;
  while (Hfind(fid, 600, DFREF_WILDCARD, &tag, &ref, &off, &len, DF_FORWARD) ==
         SUCCEED) {
    EXPECT_THAT(tag, Eq(600));
    count_tag600++;
  }
  EXPECT_THAT(count_tag600, Eq(3));

  // Search specific tag with wildcard ref (backward)
  tag = 0;
  ref = 0;
  int count_tag600_bwd = 0;
  while (Hfind(fid, 600, DFREF_WILDCARD, &tag, &ref, &off, &len, DF_BACKWARD) ==
         SUCCEED) {
    EXPECT_THAT(tag, Eq(600));
    count_tag600_bwd++;
  }
  EXPECT_THAT(count_tag600_bwd, Eq(3));

  // Search specific ref with wildcard tag (forward)
  tag = 0;
  ref = 0;
  int count_ref1 = 0;
  while (Hfind(fid, DFTAG_WILDCARD, 1, &tag, &ref, &off, &len, DF_FORWARD) ==
         SUCCEED) {
    EXPECT_THAT(ref, Eq(1));
    count_ref1++;
  }
  EXPECT_THAT(count_ref1,
              Ge(2));  // version ref 1, tag 600 ref 1, tag 601 ref 1

  // Search specific tag and specific ref
  tag = 0;
  ref = 0;
  EXPECT_THAT(Hfind(fid, 600, 2, &tag, &ref, &off, &len, DF_FORWARD),
              Eq(SUCCEED));
  EXPECT_THAT(tag, Eq(600));
  EXPECT_THAT(ref, Eq(2));
  EXPECT_THAT(len, Eq(4));

  // Search non-existent tag/ref returns FAIL
  tag = 0;
  ref = 0;
  EXPECT_THAT(Hfind(fid, 9999, 9999, &tag, &ref, &off, &len, DF_FORWARD),
              Eq(FAIL));

  // Continue search with bogus starting tag/ref returns FAIL
  tag = 8888;
  ref = 8888;
  EXPECT_THAT(Hfind(fid, DFTAG_WILDCARD, DFREF_WILDCARD, &tag, &ref, &off, &len,
                    DF_FORWARD),
              Eq(FAIL));

  // Search DFTAG_NULL with DFREF_WILDCARD (exercises lines 1554-1591)
  tag = 0;
  ref = 0;
  EXPECT_THAT(Hfind(fid, DFTAG_NULL, DFREF_WILDCARD, &tag, &ref, &off, &len,
                    DF_FORWARD),
              Eq(SUCCEED));
  // Call again to exercise ddnull != nullptr caching branch
  EXPECT_THAT(Hfind(fid, DFTAG_NULL, DFREF_WILDCARD, &tag, &ref, &off, &len,
                    DF_FORWARD),
              Eq(SUCCEED));

  // Search DFTAG_WILDCARD with non-existent ref through all blocks (exercises
  // line 1604)
  tag = 0;
  ref = 0;
  EXPECT_THAT(
      Hfind(fid, DFTAG_WILDCARD, 9999, &tag, &ref, &off, &len, DF_FORWARD),
      Eq(FAIL));

  // Search tag with special_tag != DFTAG_NULL with wildcard ref that doesn't
  // exist (exercises line 1644)
  tag = 0;
  ref = 0;
  EXPECT_THAT(
      Hfind(fid, 1234, DFREF_WILDCARD, &tag, &ref, &off, &len, DF_FORWARD),
      Eq(FAIL));

  // Search backward with non-existent tag through all blocks (exercises line
  // 1697)
  tag = 0;
  ref = 0;
  EXPECT_THAT(
      Hfind(fid, 9999, DFREF_WILDCARD, &tag, &ref, &off, &len, DF_BACKWARD),
      Eq(FAIL));

  // Search specific ref with wildcard tag (backward)
  tag = 0;
  ref = 0;
  int count_ref1_bwd = 0;
  while (Hfind(fid, DFTAG_WILDCARD, 1, &tag, &ref, &off, &len, DF_BACKWARD) ==
         SUCCEED) {
    EXPECT_THAT(ref, Eq(1));
    count_ref1_bwd++;
  }
  EXPECT_THAT(count_ref1_bwd, Eq(count_ref1));

  // Search non-existent ref with wildcard tag (backward)
  tag = 0;
  ref = 0;
  EXPECT_THAT(
      Hfind(fid, DFTAG_WILDCARD, 9999, &tag, &ref, &off, &len, DF_BACKWARD),
      Eq(FAIL));
}

TEST(HfileddTest, CheckAndReuseTagRef) {
  const std::string path = GetTempFilePath("hfiledd_check_reuse.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Write element (700, 1)
  const int32_t aid = Hstartwrite(fid, 700, 1, 4);
  ASSERT_THAT(aid, Ne(FAIL));
  uint8_t data[4] = {1, 2, 3, 4};
  EXPECT_THAT(Hwrite(aid, 4, data), Eq(4));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  // HDcheck_tagref tests
  EXPECT_THAT(HDcheck_tagref(fid, 700, 1), Eq(1));
  EXPECT_THAT(HDcheck_tagref(fid, 700, 2), Eq(0));
  EXPECT_THAT(HDcheck_tagref(fid, 999, 1), Eq(0));
  EXPECT_THAT(HDcheck_tagref(fid, DFTAG_NULL, 1), Eq(-1));
  EXPECT_THAT(HDcheck_tagref(fid, DFTAG_WILDCARD, 1), Eq(-1));
  EXPECT_THAT(HDcheck_tagref(fid, 700, DFREF_WILDCARD), Eq(-1));
  EXPECT_THAT(HDcheck_tagref(FAIL, 700, 1), Eq(-1));

  // HDreuse_tagref tests
  EXPECT_THAT(HDreuse_tagref(FAIL, 700, 1), Eq(FAIL));
  EXPECT_THAT(HDreuse_tagref(fid, DFTAG_WILDCARD, 1), Eq(FAIL));
  EXPECT_THAT(HDreuse_tagref(fid, 700, DFREF_WILDCARD), Eq(FAIL));
  EXPECT_THAT(HDreuse_tagref(fid, 999, 999), Eq(FAIL));

  // Successful reuse of (700, 1)
  EXPECT_THAT(HDreuse_tagref(fid, 700, 1), Eq(SUCCEED));

  // Verify tag/ref is still checked as existing in DD list
  EXPECT_THAT(HDcheck_tagref(fid, 700, 1), Eq(1));

  // Inquire on it: offset and length should be INVALID_OFFSET and
  // INVALID_LENGTH
  uint16 ftag = 0;
  uint16 fref = 0;
  int32_t foff = 0;
  int32_t flen = 0;
  EXPECT_THAT(Hfind(fid, 700, 1, &ftag, &fref, &foff, &flen, DF_FORWARD),
              Eq(SUCCEED));
  EXPECT_THAT(foff, Eq(INVALID_OFFSET));
  EXPECT_THAT(flen, Eq(INVALID_LENGTH));
}

TEST(HfileddTest, DuplicateAndDeleteDD) {
  const std::string path = GetTempFilePath("hfiledd_dup_del.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Write element (800, 1)
  const int32_t aid = Hstartwrite(fid, 800, 1, 8);
  ASSERT_THAT(aid, Ne(FAIL));
  uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  EXPECT_THAT(Hwrite(aid, 8, data), Eq(8));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  // Duplicate (800, 1) to (800, 2)
  EXPECT_THAT(Hdupdd(fid, 800, 2, 800, 1), Eq(SUCCEED));

  // Duplicate to already existing tag/ref fails
  EXPECT_THAT(Hdupdd(fid, 800, 2, 800, 1), Eq(FAIL));

  // Duplicate from non-existent tag/ref fails
  EXPECT_THAT(Hdupdd(fid, 800, 3, 9999, 9999), Eq(FAIL));

  // Duplicate with bad fid fails
  EXPECT_THAT(Hdupdd(FAIL, 800, 3, 800, 1), Eq(FAIL));

  // Verify duplicated element (800, 2) has same offset and length as (800, 1)
  uint16 t1 = 0, r1 = 0, t2 = 0, r2 = 0;
  int32_t off1 = 0, len1 = 0, off2 = 0, len2 = 0;
  EXPECT_THAT(Hfind(fid, 800, 1, &t1, &r1, &off1, &len1, DF_FORWARD),
              Eq(SUCCEED));
  EXPECT_THAT(Hfind(fid, 800, 2, &t2, &r2, &off2, &len2, DF_FORWARD),
              Eq(SUCCEED));
  EXPECT_THAT(off1, Eq(off2));
  EXPECT_THAT(len1, Eq(len2));

  // Test Hdeldd
  EXPECT_THAT(Hdeldd(FAIL, 800, 2), Eq(FAIL));
  EXPECT_THAT(Hdeldd(fid, DFTAG_WILDCARD, 2), Eq(FAIL));
  EXPECT_THAT(Hdeldd(fid, 800, DFREF_WILDCARD), Eq(FAIL));
  EXPECT_THAT(Hdeldd(fid, 9999, 9999), Eq(FAIL));

  // Delete (800, 2)
  EXPECT_THAT(Hdeldd(fid, 800, 2), Eq(SUCCEED));
  EXPECT_THAT(HDcheck_tagref(fid, 800, 2), Eq(0));

  // Now duplicating to (800, 2) succeeds again!
  EXPECT_THAT(Hdupdd(fid, 800, 2, 800, 1), Eq(SUCCEED));
  EXPECT_THAT(HDcheck_tagref(fid, 800, 2), Eq(1));
}

TEST(HfileddTest, DirectHTPAPIs) {
  const std::string path = GetTempFilePath("hfiledd_direct_htp.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  filerec_t* file_rec = static_cast<filerec_t*>(HAatom_object(fid));
  ASSERT_THAT(file_rec, NotNull());

  // HTPcreate argument validation
  EXPECT_THAT(HTPcreate(nullptr, 900, 1), Eq(FAIL));
  EXPECT_THAT(HTPcreate(file_rec, DFTAG_NULL, 1), Eq(FAIL));
  EXPECT_THAT(HTPcreate(file_rec, DFTAG_WILDCARD, 1), Eq(FAIL));
  EXPECT_THAT(HTPcreate(file_rec, 900, DFREF_WILDCARD), Eq(FAIL));

  // Create DD (900, 1)
  const atom_t ddid = HTPcreate(file_rec, 900, 1);
  ASSERT_THAT(ddid, Ne(FAIL));

  // Duplicate create fails
  EXPECT_THAT(HTPcreate(file_rec, 900, 1), Eq(FAIL));

  // HTPinquire
  EXPECT_THAT(HTPinquire(FAIL, nullptr, nullptr, nullptr, nullptr), Eq(FAIL));
  uint16 tag = 0, ref = 0;
  int32_t off = 0, len = 0;
  EXPECT_THAT(HTPinquire(ddid, &tag, &ref, &off, &len), Eq(SUCCEED));
  EXPECT_THAT(tag, Eq(900));
  EXPECT_THAT(ref, Eq(1));
  EXPECT_THAT(off, Eq(INVALID_OFFSET));
  EXPECT_THAT(len, Eq(INVALID_LENGTH));

  // Inquire with all null pointers
  EXPECT_THAT(HTPinquire(ddid, nullptr, nullptr, nullptr, nullptr),
              Eq(SUCCEED));

  // HTPis_special on regular tag
  EXPECT_THAT(HTPis_special(FAIL), Eq(FALSE));
  EXPECT_THAT(HTPis_special(ddid), Eq(FALSE));

  // HTPupdate
  EXPECT_THAT(HTPupdate(FAIL, 100, 50), Eq(FAIL));
  EXPECT_THAT(HTPupdate(ddid, 1024, 256), Eq(SUCCEED));
  EXPECT_THAT(HTPinquire(ddid, nullptr, nullptr, &off, &len), Eq(SUCCEED));
  EXPECT_THAT(off, Eq(1024));
  EXPECT_THAT(len, Eq(256));

  // Update offset only (-2 for length)
  EXPECT_THAT(HTPupdate(ddid, 2048, -2), Eq(SUCCEED));
  EXPECT_THAT(HTPinquire(ddid, nullptr, nullptr, &off, &len), Eq(SUCCEED));
  EXPECT_THAT(off, Eq(2048));
  EXPECT_THAT(len, Eq(256));

  // Update length only (-2 for offset)
  EXPECT_THAT(HTPupdate(ddid, -2, 512), Eq(SUCCEED));
  EXPECT_THAT(HTPinquire(ddid, nullptr, nullptr, &off, &len), Eq(SUCCEED));
  EXPECT_THAT(off, Eq(2048));
  EXPECT_THAT(len, Eq(512));

  // Test HTPupdate with caching turned off (exercises HTIupdate_dd disk write)
  EXPECT_THAT(Hcache(fid, 0), Eq(SUCCEED));
  EXPECT_THAT(HTPupdate(ddid, 100000, 2048), Eq(SUCCEED));
  EXPECT_THAT(HTPinquire(ddid, nullptr, nullptr, &off, &len), Eq(SUCCEED));
  EXPECT_THAT(off, Eq(100000));
  EXPECT_THAT(len, Eq(2048));

  // Exercise HPseek failure in HTIupdate_dd (lines 1754 and 756)
  const int32_t orig_myoffset = file_rec->ddhead->myoffset;
  file_rec->ddhead->myoffset = -1000;
  EXPECT_THAT(HTPupdate(ddid, 50, 10), Eq(FAIL));
  file_rec->ddhead->myoffset = orig_myoffset;
  EXPECT_THAT(Hcache(fid, 1), Eq(SUCCEED));

  // End access to the created DD before selecting it
  EXPECT_THAT(HTPendaccess(FAIL), Eq(FAIL));
  EXPECT_THAT(HTPendaccess(ddid), Eq(SUCCEED));

  // HTPselect
  EXPECT_THAT(HTPselect(nullptr, 900, 1), Eq(FAIL));
  EXPECT_THAT(HTPselect(file_rec, DFTAG_NULL, 1), Eq(FAIL));
  EXPECT_THAT(HTPselect(file_rec, 900, DFREF_WILDCARD), Eq(FAIL));
  EXPECT_THAT(HTPselect(file_rec, 9999, 9999), Eq(FAIL));
  const atom_t sel_ddid = HTPselect(file_rec, 900, 1);
  ASSERT_THAT(sel_ddid, Ne(FAIL));

  // HTPdelete
  EXPECT_THAT(HTPdelete(FAIL), Eq(FAIL));
  EXPECT_THAT(HTPdelete(sel_ddid), Eq(SUCCEED));

  // Special tag create & test HTPis_special
  const uint16 sp_tag = MKSPECIALTAG(901);
  const atom_t sp_ddid = HTPcreate(file_rec, sp_tag, 1);
  ASSERT_THAT(sp_ddid, Ne(FAIL));
  EXPECT_THAT(HTPis_special(sp_ddid), Eq(TRUE));
  EXPECT_THAT(HTPendaccess(sp_ddid), Eq(SUCCEED));

  // Tampering with dd_ptr->tag to trigger HTIunregister_tag_ref failure
  const atom_t err_ddid = HTPcreate(file_rec, 950, 1);
  ASSERT_THAT(err_ddid, Ne(FAIL));
  dd_t* err_dd = static_cast<dd_t*>(HAatom_object(err_ddid));
  ASSERT_THAT(err_dd, NotNull());
  err_dd->tag = 9999;
  EXPECT_THAT(HTPdelete(err_ddid), Eq(FAIL));
  // Restore tag so we can cleanly delete it
  err_dd->tag = 950;
  EXPECT_THAT(HTPdelete(err_ddid), Eq(SUCCEED));

  // Tamper with ddnull/ddhead to trigger HTInew_dd_block failure in HTPcreate
  ddblock_t* orig_head = file_rec->ddhead;
  ddblock_t* orig_null = file_rec->ddnull;
  int orig_null_idx = file_rec->ddnull_idx;
  file_rec->ddnull = nullptr;
  file_rec->ddnull_idx = -1;
  file_rec->ddhead = nullptr;
  EXPECT_THAT(HTPcreate(file_rec, 951, 1), Eq(FAIL));
  file_rec->ddhead = orig_head;
  file_rec->ddnull = orig_null;
  file_rec->ddnull_idx = orig_null_idx;

  // Htagnewref when bv_find_next_zero fails
  const uint16 t960 = 960;
  EXPECT_THAT(Htagnewref(fid, t960), Eq(1));
  const atom_t dd960 = HTPcreate(file_rec, t960, 1);
  ASSERT_THAT(dd960, Ne(FAIL));
  tag_info** tip = reinterpret_cast<tag_info**>(
      tbbtdfind(file_rec->tag_tree, const_cast<uint16*>(&t960), nullptr));
  ASSERT_THAT(tip, NotNull());
  bv_ptr orig_b = (*tip)->b;
  (*tip)->b = nullptr;
  EXPECT_THAT(Htagnewref(fid, t960), Eq(0));
  (*tip)->b = orig_b;

  // HTIunregister_tag_ref when ref_bit == BV_FALSE
  bv_set((*tip)->b, 1, BV_FALSE);
  EXPECT_THAT(HTPdelete(dd960), Eq(FAIL));
  bv_set((*tip)->b, 1, BV_TRUE);
  EXPECT_THAT(HTPdelete(dd960), Eq(SUCCEED));

  // HTIunregister_tag_ref when bv_get fails (ref >= bv_size)
  const uint16 t970 = 970;
  const atom_t dd970 = HTPcreate(file_rec, t970, 1);
  ASSERT_THAT(dd970, Ne(FAIL));
  dd_t* d970 = static_cast<dd_t*>(HAatom_object(dd970));
  ASSERT_THAT(d970, NotNull());
  d970->ref = 60000;
  EXPECT_THAT(HTPdelete(dd970), Eq(FAIL));
  d970->ref = 1;
  EXPECT_THAT(HTPdelete(dd970), Eq(SUCCEED));

  // Test Hdeldd failure when HTPdelete fails (exercises lines 1256-1258)
  const uint16 t980 = 980;
  const atom_t dd980 = HTPcreate(file_rec, t980, 1);
  ASSERT_THAT(dd980, Ne(FAIL));
  EXPECT_THAT(HTPendaccess(dd980), Eq(SUCCEED));
  tag_info** tip980 = reinterpret_cast<tag_info**>(
      tbbtdfind(file_rec->tag_tree, const_cast<uint16*>(&t980), nullptr));
  ASSERT_THAT(tip980, NotNull());
  bv_set((*tip980)->b, 1, BV_FALSE);
  EXPECT_THAT(Hdeldd(fid, t980, 1), Eq(FAIL));
  bv_set((*tip980)->b, 1, BV_TRUE);
  EXPECT_THAT(Hdeldd(fid, t980, 1), Eq(SUCCEED));
}

TEST(HfileddTest, MultipleDDBlocksChainedCachingAndNonCaching) {
  // Test 1: Caching enabled (default)
  const std::string path_cached = GetTempFilePath("hfiledd_blocks_cached.hdf");
  // ndds = 2 forces new blocks quickly!
  const int32_t fid1 = Hopen(path_cached.c_str(), DFACC_CREATE, 2);
  ASSERT_THAT(fid1, Ne(FAIL));

  uint8_t data[4] = {1, 2, 3, 4};
  // Writing 10 elements with ndds=2 allocates at least 5 DD blocks!
  for (uint16 ref = 1; ref <= 10; ++ref) {
    const int32_t aid = Hstartwrite(fid1, 1000, ref, 4);
    ASSERT_THAT(aid, Ne(FAIL));
    EXPECT_THAT(Hwrite(aid, 4, data), Eq(4));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  }
  EXPECT_THAT(Hclose(fid1), Eq(SUCCEED));

  // Reopen and read from disk: tests HTPstart decoding multiple chained DD
  // blocks!
  const int32_t rfid1 = Hopen(path_cached.c_str(), DFACC_READ, 0);
  ASSERT_THAT(rfid1, Ne(FAIL));
  EXPECT_THAT(Hnumber(rfid1, 1000), Eq(10));

  // Search forward across all blocks (exercises idx = 0 line 1555)
  uint16 tag = 0, ref = 0;
  int32_t off = 0, len = 0;
  int count_fwd = 0;
  while (Hfind(rfid1, DFTAG_WILDCARD, DFREF_WILDCARD, &tag, &ref, &off, &len,
               DF_FORWARD) == SUCCEED) {
    count_fwd++;
  }
  EXPECT_THAT(count_fwd, Ge(10));

  // Search backward across all blocks
  tag = 0;
  ref = 0;
  int count_bwd = 0;
  while (Hfind(rfid1, DFTAG_WILDCARD, DFREF_WILDCARD, &tag, &ref, &off, &len,
               DF_BACKWARD) == SUCCEED) {
    count_bwd++;
  }
  EXPECT_THAT(count_bwd, Eq(count_fwd));

  // Search specific tag with wildcard ref across blocks (exercises line 1644)
  tag = 0;
  ref = 0;
  int count_t1000 = 0;
  while (Hfind(rfid1, 1000, DFREF_WILDCARD, &tag, &ref, &off, &len,
               DF_FORWARD) == SUCCEED) {
    count_t1000++;
  }
  EXPECT_THAT(count_t1000, Eq(10));

  // Search specific ref with wildcard tag across blocks (exercises line 1604)
  tag = 0;
  ref = 0;
  int count_r1 = 0;
  while (Hfind(rfid1, DFTAG_WILDCARD, 1, &tag, &ref, &off, &len, DF_FORWARD) ==
         SUCCEED) {
    count_r1++;
  }
  EXPECT_THAT(count_r1, Ge(2));

  // Search empty DDs across blocks (exercises line 1585)
  tag = 0;
  ref = 0;
  int count_null = 0;
  while (Hfind(rfid1, DFTAG_NULL, DFREF_WILDCARD, &tag, &ref, &off, &len,
               DF_FORWARD) == SUCCEED) {
    count_null++;
  }
  EXPECT_THAT(count_null, Ge(0));

  EXPECT_THAT(Hclose(rfid1), Eq(SUCCEED));

  // Test 2: Caching disabled (file_rec->cache == 0)
  const std::string path_nocache =
      GetTempFilePath("hfiledd_blocks_nocache.hdf");
  const int32_t fid2 = Hopen(path_nocache.c_str(), DFACC_CREATE, 2);
  ASSERT_THAT(fid2, Ne(FAIL));
  EXPECT_THAT(Hcache(fid2, 0), Eq(SUCCEED));

  for (uint16 ref = 1; ref <= 8; ++ref) {
    const int32_t aid = Hstartwrite(fid2, 1001, ref, 4);
    ASSERT_THAT(aid, Ne(FAIL));
    EXPECT_THAT(Hwrite(aid, 4, data), Eq(4));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  }
  EXPECT_THAT(Hclose(fid2), Eq(SUCCEED));

  // Reopen and verify
  const int32_t rfid2 = Hopen(path_nocache.c_str(), DFACC_READ, 0);
  ASSERT_THAT(rfid2, Ne(FAIL));
  EXPECT_THAT(Hnumber(rfid2, 1001), Eq(8));
  EXPECT_THAT(Hclose(rfid2), Eq(SUCCEED));
}

TEST(HfileddTest, OddBlockSizeAndSpecialTagCount) {
  // ndds = 5 is odd (> MIN_NDDS=4), exercising the `block->ndds % 2 == 1`
  // branch in HTIcount_dd!
  const std::string path = GetTempFilePath("hfiledd_odd_ndds.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 5);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  filerec_t* file_rec = static_cast<filerec_t*>(HAatom_object(fid));
  ASSERT_THAT(file_rec, NotNull());

  // Create elements with special tag
  const uint16 base_tag = 1100;
  const uint16 sp_tag = MKSPECIALTAG(base_tag);
  for (uint16 ref = 1; ref <= 5; ++ref) {
    const atom_t ddid = HTPcreate(file_rec, sp_tag, ref);
    ASSERT_THAT(ddid, Ne(FAIL));
    EXPECT_THAT(HTPupdate(ddid, 100 * ref, 10), Eq(SUCCEED));
    EXPECT_THAT(HTPendaccess(ddid), Eq(SUCCEED));
  }

  // Count base tag: exercises count when special tag exists
  EXPECT_THAT(Hnumber(fid, base_tag), Eq(5));

  // Count version element on odd block size (index 0 is DFTAG_VERSION)
  EXPECT_THAT(Hnumber(fid, DFTAG_VERSION), Eq(1));

  // Tag with high bit set (special_tag == DFTAG_NULL)
  const uint16 high_tag = 0x8001;
  const atom_t hdd = HTPcreate(file_rec, high_tag, 1);
  ASSERT_THAT(hdd, Ne(FAIL));
  EXPECT_THAT(HTPupdate(hdd, 50, 5), Eq(SUCCEED));
  EXPECT_THAT(HTPendaccess(hdd), Eq(SUCCEED));

  // Count high tag with wildcard ref (exercises lines 1848-1856)
  EXPECT_THAT(Hnumber(fid, high_tag), Eq(1));
  EXPECT_THAT(Hnumber(fid, 0x8002), Eq(0));

  // Find high tag with wildcard ref (exercises lines 1613-1630)
  uint16 ftag = 0, fref = 0;
  int32_t foff = 0, flen = 0;
  EXPECT_THAT(Hfind(fid, high_tag, DFREF_WILDCARD, &ftag, &fref, &foff, &flen,
                    DF_FORWARD),
              Eq(SUCCEED));
  EXPECT_THAT(ftag, Eq(high_tag));
  EXPECT_THAT(fref, Eq(1));

  // Find high tag that doesn't exist
  ftag = 0;
  fref = 0;
  EXPECT_THAT(Hfind(fid, 0x8002, DFREF_WILDCARD, &ftag, &fref, &foff, &flen,
                    DF_FORWARD),
              Eq(FAIL));

  // Find high tag with wildcard ref backward
  ftag = 0;
  fref = 0;
  EXPECT_THAT(Hfind(fid, high_tag, DFREF_WILDCARD, &ftag, &fref, &foff, &flen,
                    DF_BACKWARD),
              Eq(SUCCEED));
  EXPECT_THAT(ftag, Eq(high_tag));
  EXPECT_THAT(fref, Eq(1));

  // Find non-existent high tag backward
  ftag = 0;
  fref = 0;
  EXPECT_THAT(Hfind(fid, 0x8002, DFREF_WILDCARD, &ftag, &fref, &foff, &flen,
                    DF_BACKWARD),
              Eq(FAIL));

  // Sync test
  EXPECT_THAT(HTPsync(file_rec), Eq(SUCCEED));
}

TEST(HfileddTest, CorruptFileHeadersAndDDs) {
  const uint8_t magic[4] = {0x0e, 0x03, 0x13, 0x01};

  // Truncated file with only magic header (exercises line 199 in HTPstart)
  const std::string path_magic_only = GetTempFilePath("hfiledd_magic_only.hdf");
  FILE* fp = fopen(path_magic_only.c_str(), "wb");
  ASSERT_THAT(fp, NotNull());
  fwrite(magic, 1, 4, fp);
  fclose(fp);
  EXPECT_THAT(Hopen(path_magic_only.c_str(), DFACC_READ, 0), Eq(FAIL));

  // 1. File with ndds <= 0
  const std::string path_zero_ndds = GetTempFilePath("hfiledd_zero_ndds.hdf");
  fp = fopen(path_zero_ndds.c_str(), "wb");
  ASSERT_THAT(fp, NotNull());
  fwrite(magic, 1, 4, fp);
  const uint8_t hdr[6] = {0, 0, 0, 0, 0, 0};
  fwrite(hdr, 1, 6, fp);
  fclose(fp);

  EXPECT_THAT(Hopen(path_zero_ndds.c_str(), DFACC_READ, 0), Eq(FAIL));

  // Truncated DD block data (ndds = 2, but only 6 bytes of DD data exist)
  const std::string path_trunc_dd = GetTempFilePath("hfiledd_trunc_dd.hdf");
  fp = fopen(path_trunc_dd.c_str(), "wb");
  ASSERT_THAT(fp, NotNull());
  fwrite(magic, 1, 4, fp);
  const uint8_t hdr_trunc[6] = {0, 2, 0, 0, 0, 0};
  fwrite(hdr_trunc, 1, 6, fp);
  const uint8_t partial_dd[6] = {1, 2, 3, 4, 5, 6};
  fwrite(partial_dd, 1, 6, fp);
  fclose(fp);
  EXPECT_THAT(Hopen(path_trunc_dd.c_str(), DFACC_READ, 0), Eq(FAIL));

  // File where block 1 has negative nextoffset (exercises line 193 in HTPstart)
  const std::string path_neg_next = GetTempFilePath("hfiledd_neg_next.hdf");
  fp = fopen(path_neg_next.c_str(), "wb");
  ASSERT_THAT(fp, NotNull());
  fwrite(magic, 1, 4, fp);
  uint8_t hdr_neg[6];
  uint8_t* p = hdr_neg;
  INT16ENCODE(p, 1);
  INT32ENCODE(p, -1);  // nextoffset = -1
  fwrite(hdr_neg, 1, 6, fp);
  uint8_t neg_dd[12];
  p = neg_dd;
  EncodeDD(p, 100, 1, 100, 4);
  fwrite(neg_dd, 1, 12, fp);
  fclose(fp);
  EXPECT_THAT(Hopen(path_neg_next.c_str(), DFACC_READ, 0), Eq(FAIL));

  // Duplicate (tag, ref) in file DD block (exercises line 250 and line 1954)
  const std::string path_dup_tagref = GetTempFilePath("hfiledd_dup_tagref.hdf");
  fp = fopen(path_dup_tagref.c_str(), "wb");
  ASSERT_THAT(fp, NotNull());
  fwrite(magic, 1, 4, fp);
  const uint8_t hdr_dup[6] = {0, 2, 0, 0, 0, 0};
  fwrite(hdr_dup, 1, 6, fp);
  uint8_t dup_buf[12];
  uint8_t* dp = dup_buf;
  EncodeDD(dp, 100, 1, 100, 4);
  fwrite(dup_buf, 1, 12, fp);
  dp = dup_buf;
  EncodeDD(dp, 100, 1, 104, 4);  // Same (100, 1) duplicate
  fwrite(dup_buf, 1, 12, fp);
  fclose(fp);
  EXPECT_THAT(Hopen(path_dup_tagref.c_str(), DFACC_READ, 0), Eq(FAIL));

  // 2. File with DD whose offset + length overflows INT32_MAX
  const std::string path_ovfl = GetTempFilePath("hfiledd_overflow.hdf");
  fp = fopen(path_ovfl.c_str(), "wb");
  ASSERT_THAT(fp, NotNull());
  fwrite(magic, 1, 4, fp);
  const uint8_t hdr1[6] = {0, 1, 0, 0, 0, 0};
  fwrite(hdr1, 1, 6, fp);
  uint8_t dd_buf[12];
  p = dd_buf;
  UINT16ENCODE(p, 100);
  UINT16ENCODE(p, 1);
  INT32ENCODE(p, 0x7ffffff0);
  INT32ENCODE(p, 0x100);
  fwrite(dd_buf, 1, 12, fp);
  fclose(fp);

  EXPECT_THAT(Hopen(path_ovfl.c_str(), DFACC_READ, 0), Eq(FAIL));

  // 3. File with increasing block sizes (ndds: 1 -> 2 -> 3) to trigger tbuf
  // reallocations in HTPstart
  const std::string path_grow = GetTempFilePath("hfiledd_grow_ndds.hdf");
  fp = fopen(path_grow.c_str(), "wb");
  ASSERT_THAT(fp, NotNull());
  fwrite(magic, 1, 4, fp);

  // Block 1: ndds=1, nextoffset=22
  uint8_t b1_hdr[6];
  p = b1_hdr;
  INT16ENCODE(p, 1);
  INT32ENCODE(p, 22);
  fwrite(b1_hdr, 1, 6, fp);
  p = dd_buf;
  EncodeDD(p, 100, 1, 100, 4);
  fwrite(dd_buf, 1, 12, fp);

  // Block 2: ndds=2, nextoffset=52
  uint8_t b2_hdr[6];
  p = b2_hdr;
  INT16ENCODE(p, 2);
  INT32ENCODE(p, 52);
  fwrite(b2_hdr, 1, 6, fp);
  p = dd_buf;
  EncodeDD(p, 100, 2, 104, 4);
  fwrite(dd_buf, 1, 12, fp);
  p = dd_buf;
  EncodeDD(p, 100, 3, 108, 4);
  fwrite(dd_buf, 1, 12, fp);

  // Block 3: ndds=3, nextoffset=0
  uint8_t b3_hdr[6];
  p = b3_hdr;
  INT16ENCODE(p, 3);
  INT32ENCODE(p, 0);
  fwrite(b3_hdr, 1, 6, fp);
  p = dd_buf;
  EncodeDD(p, 100, 4, 112, 4);
  fwrite(dd_buf, 1, 12, fp);
  p = dd_buf;
  EncodeDD(p, 100, 5, 116, 4);
  fwrite(dd_buf, 1, 12, fp);
  p = dd_buf;
  EncodeDD(p, 100, 6, 120, 4);
  fwrite(dd_buf, 1, 12, fp);

  fclose(fp);

  const int32_t grow_fid = Hopen(path_grow.c_str(), DFACC_READ, 0);
  ASSERT_THAT(grow_fid, Ne(FAIL));
  EXPECT_THAT(Hnumber(grow_fid, 100), Eq(6));
  EXPECT_THAT(Hclose(grow_fid), Eq(SUCCEED));
}

TEST(HfileddTest, HTPinitAndSyncEdgeCases) {
  EXPECT_THAT(HTPinit(nullptr, 0), Eq(FAIL));
  EXPECT_THAT(HTPinit(nullptr, -1), Eq(FAIL));

  const std::string path = GetTempFilePath("hfiledd_sync_edge.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));

  filerec_t* file_rec = static_cast<filerec_t*>(HAatom_object(fid));
  ASSERT_THAT(file_rec, NotNull());

  // HTPsync with ddhead == nullptr
  ddblock_t* orig_head = file_rec->ddhead;
  file_rec->ddhead = nullptr;
  EXPECT_THAT(HTPsync(file_rec), Eq(FAIL));
  file_rec->ddhead = orig_head;

  EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
}

TEST(HfileddTest, BadFileRecordTests) {
  const std::string path = GetTempFilePath("hfiledd_bad_frec.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  filerec_t* file_rec = static_cast<filerec_t*>(HAatom_object(fid));
  ASSERT_THAT(file_rec, NotNull());

  filerec_t bad_frec = *file_rec;
  bad_frec.refcount = 0;
  const int32_t bad_fid = HAregister_atom(FIDGROUP, &bad_frec);
  ASSERT_THAT(bad_fid, Ne(FAIL));

  uint16 tag = 0, ref = 0;
  int32_t off = 0, len = 0;
  EXPECT_THAT(Hfind(bad_fid, DFTAG_WILDCARD, DFREF_WILDCARD, &tag, &ref, &off,
                    &len, DF_FORWARD),
              Eq(FAIL));
  EXPECT_THAT(Hdupdd(bad_fid, 100, 2, 100, 1), Eq(FAIL));
  EXPECT_THAT(Hnumber(bad_fid, DFTAG_WILDCARD), Eq(FAIL));
  EXPECT_THAT(Hnewref(bad_fid), Eq(0));
  EXPECT_THAT(Htagnewref(bad_fid, 100), Eq(0));
  EXPECT_THAT(HDcheck_tagref(bad_fid, 100, 1), Eq(0));
  EXPECT_THAT(HDreuse_tagref(bad_fid, 100, 1), Eq(FAIL));
  EXPECT_THAT(Hdeldd(bad_fid, 100, 1), Eq(FAIL));

  EXPECT_THAT(HAremove_atom(bad_fid), NotNull());
}

}  // namespace
