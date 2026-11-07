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

// Tests for the multi-file annotation interface (hdf/src/mfan.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/mfan.c
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/mfan.h
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/test/man.c

#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

// GoogleTest's gtest.h defines SUCCEED() and FAIL() as function-like macros.
// Undefine them before including HDF4 headers so hdf.h can define its constants
// SUCCEED (0) and FAIL (-1) without macro redefinition errors under no_modules.
#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/htags.h"
#include "third_party/hdf4/hdf/src/mfan.h"
#include "third_party/hdf4/hdf/src/mfan_priv.h"

namespace hdf4 {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::Lt;
using ::testing::Ne;
using ::testing::NotNull;
using ::testing::StrEq;

std::string GetTempFilePath(std::string_view filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

TEST(MfanTest, TypeAndTagConversions) {
  // ANatype2tag: enum to tag
  EXPECT_THAT(ANatype2tag(AN_FILE_LABEL), Eq(DFTAG_FID));
  EXPECT_THAT(ANatype2tag(AN_FILE_DESC), Eq(DFTAG_FD));
  EXPECT_THAT(ANatype2tag(AN_DATA_LABEL), Eq(DFTAG_DIL));
  EXPECT_THAT(ANatype2tag(AN_DATA_DESC), Eq(DFTAG_DIA));
  EXPECT_THAT(ANatype2tag(static_cast<ann_type>(999)), Eq(DFTAG_NULL));

  // ANtag2atype: tag to enum
  EXPECT_THAT(ANtag2atype(DFTAG_FID), Eq(AN_FILE_LABEL));
  EXPECT_THAT(ANtag2atype(DFTAG_FD), Eq(AN_FILE_DESC));
  EXPECT_THAT(ANtag2atype(DFTAG_DIL), Eq(AN_DATA_LABEL));
  EXPECT_THAT(ANtag2atype(DFTAG_DIA), Eq(AN_DATA_DESC));
  EXPECT_THAT(ANtag2atype(DFTAG_NULL), Eq(static_cast<ann_type>(FAIL)));
  EXPECT_THAT(ANtag2atype(9999), Eq(static_cast<ann_type>(FAIL)));
}

TEST(MfanTest, StartAndEndArgumentValidation) {
  EXPECT_THAT(ANstart(FAIL), Eq(FAIL));
  EXPECT_THAT(ANend(FAIL), Eq(FAIL));

  int32 n_fl = 0, n_fd = 0, n_dl = 0, n_dd = 0;
  EXPECT_THAT(ANfileinfo(FAIL, &n_fl, &n_fd, &n_dl, &n_dd), Eq(FAIL));
  EXPECT_THAT(ANfileinfo(FAIL, nullptr, nullptr, nullptr, nullptr), Eq(FAIL));
}

TEST(MfanTest, FileAnnotationsLifecycleAndPersistence) {
  const std::string path = GetTempFilePath("mfan_file_ann.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));

  const int32_t an_id = ANstart(fid);
  ASSERT_THAT(an_id, Ne(FAIL));

  // Initial fileinfo: all zero
  int32 n_fl = -1, n_fd = -1, n_dl = -1, n_dd = -1;
  EXPECT_THAT(ANfileinfo(an_id, &n_fl, &n_fd, &n_dl, &n_dd), Eq(SUCCEED));
  EXPECT_THAT(n_fl, Eq(0));
  EXPECT_THAT(n_fd, Eq(0));
  EXPECT_THAT(n_dl, Eq(0));
  EXPECT_THAT(n_dd, Eq(0));

  // 1. Create file label
  const int32_t fl_id = ANcreatef(an_id, AN_FILE_LABEL);
  ASSERT_THAT(fl_id, Ne(FAIL));
  constexpr std::string_view kFileLabel = "Sample HDF File Label";
  EXPECT_THAT(ANwriteann(fl_id, kFileLabel.data(),
                         static_cast<int32>(kFileLabel.size())),
              Eq(SUCCEED));
  EXPECT_THAT(ANannlen(fl_id), Eq(static_cast<int32>(kFileLabel.size())));

  std::vector<char> fl_buf(kFileLabel.size() + 1, '\0');
  EXPECT_THAT(
      ANreadann(fl_id, fl_buf.data(), static_cast<int32>(fl_buf.size())),
      Eq(SUCCEED));
  EXPECT_THAT(std::string_view(fl_buf.data()), Eq(kFileLabel));

  // Query tag/ref of the annotation
  uint16 ann_tag = 0, ann_ref = 0;
  EXPECT_THAT(ANid2tagref(fl_id, &ann_tag, &ann_ref), Eq(SUCCEED));
  EXPECT_THAT(ann_tag, Eq(DFTAG_FID));
  EXPECT_THAT(ann_ref, Gt(0));

  // Test ANtagref2id
  const int32_t id_by_tr = ANtagref2id(an_id, ann_tag, ann_ref);
  EXPECT_THAT(id_by_tr, Eq(fl_id));

  // Test ANget_tagref for index 0
  uint16 get_tag = 0, get_ref = 0;
  EXPECT_THAT(ANget_tagref(an_id, 0, AN_FILE_LABEL, &get_tag, &get_ref),
              Eq(SUCCEED));
  EXPECT_THAT(get_tag, Eq(ann_tag));
  EXPECT_THAT(get_ref, Eq(ann_ref));

  // 2. Create file description
  const int32_t fd_id = ANcreatef(an_id, AN_FILE_DESC);
  ASSERT_THAT(fd_id, Ne(FAIL));
  constexpr std::string_view kFileDesc =
      "This is a longer multi-line file description.\nContains extra metadata.";
  EXPECT_THAT(
      ANwriteann(fd_id, kFileDesc.data(), static_cast<int32>(kFileDesc.size())),
      Eq(SUCCEED));
  EXPECT_THAT(ANannlen(fd_id), Eq(static_cast<int32>(kFileDesc.size())));

  std::vector<char> fd_buf(kFileDesc.size() + 1, '\0');
  EXPECT_THAT(
      ANreadann(fd_id, fd_buf.data(), static_cast<int32>(fd_buf.size())),
      Eq(SUCCEED));
  EXPECT_THAT(std::string_view(fd_buf.data()), Eq(kFileDesc));

  // Test rewriting an existing annotation (HDreuse_tagref)
  constexpr std::string_view kUpdatedFileLabel = "Updated File Label";
  EXPECT_THAT(ANwriteann(fl_id, kUpdatedFileLabel.data(),
                         static_cast<int32>(kUpdatedFileLabel.size())),
              Eq(SUCCEED));
  EXPECT_THAT(ANannlen(fl_id),
              Eq(static_cast<int32>(kUpdatedFileLabel.size())));
  std::vector<char> updated_fl_buf(kUpdatedFileLabel.size() + 1, '\0');
  EXPECT_THAT(ANreadann(fl_id, updated_fl_buf.data(),
                        static_cast<int32>(updated_fl_buf.size())),
              Eq(SUCCEED));
  EXPECT_THAT(std::string_view(updated_fl_buf.data()), Eq(kUpdatedFileLabel));

  // Fileinfo now has 1 label and 1 desc
  EXPECT_THAT(ANfileinfo(an_id, &n_fl, &n_fd, &n_dl, &n_dd), Eq(SUCCEED));
  EXPECT_THAT(n_fl, Eq(1));
  EXPECT_THAT(n_fd, Eq(1));
  EXPECT_THAT(n_dl, Eq(0));
  EXPECT_THAT(n_dd, Eq(0));

  // End access to annotations
  EXPECT_THAT(ANendaccess(fl_id), Eq(SUCCEED));
  EXPECT_THAT(ANendaccess(fd_id), Eq(SUCCEED));

  // Close annotation interface and file
  EXPECT_THAT(ANend(an_id), Eq(SUCCEED));
  EXPECT_THAT(Hclose(fid), Eq(SUCCEED));

  // 3. Re-open and verify file annotations persisted to disk
  const int32_t rfid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_THAT(rfid, Ne(FAIL));
  const int32_t ran_id = ANstart(rfid);
  ASSERT_THAT(ran_id, Ne(FAIL));

  // First call creates trees from disk
  EXPECT_THAT(ANfileinfo(ran_id, &n_fl, &n_fd, &n_dl, &n_dd), Eq(SUCCEED));
  EXPECT_THAT(n_fl, Eq(1));
  EXPECT_THAT(n_fd, Eq(1));
  EXPECT_THAT(n_dl, Eq(0));
  EXPECT_THAT(n_dd, Eq(0));

  // Second call exercises already-created trees branch
  EXPECT_THAT(ANfileinfo(ran_id, &n_fl, &n_fd, &n_dl, &n_dd), Eq(SUCCEED));
  EXPECT_THAT(n_fl, Eq(1));
  EXPECT_THAT(n_fd, Eq(1));

  // Select label by index 0
  const int32_t sel_fl = ANselect(ran_id, 0, AN_FILE_LABEL);
  ASSERT_THAT(sel_fl, Ne(FAIL));
  std::vector<char> r_fl_buf(ANannlen(sel_fl) + 1, '\0');
  EXPECT_THAT(
      ANreadann(sel_fl, r_fl_buf.data(), static_cast<int32>(r_fl_buf.size())),
      Eq(SUCCEED));
  EXPECT_THAT(std::string_view(r_fl_buf.data()), Eq(kUpdatedFileLabel));

  // Test label truncation with maxlen < ann_len + 1
  std::vector<char> trunc_label_buf(5, '\0');
  EXPECT_THAT(ANreadann(sel_fl, trunc_label_buf.data(), 4), Eq(SUCCEED));
  EXPECT_THAT(ANendaccess(sel_fl), Eq(SUCCEED));

  // Select desc by index 0
  const int32_t sel_fd = ANselect(ran_id, 0, AN_FILE_DESC);
  ASSERT_THAT(sel_fd, Ne(FAIL));
  std::vector<char> r_fd_buf(ANannlen(sel_fd) + 1, '\0');
  EXPECT_THAT(
      ANreadann(sel_fd, r_fd_buf.data(), static_cast<int32>(r_fd_buf.size())),
      Eq(SUCCEED));
  EXPECT_THAT(std::string_view(r_fd_buf.data()), Eq(kFileDesc));

  // Test desc truncation with maxlen < ann_len
  std::vector<char> trunc_desc_buf(5, '\0');
  EXPECT_THAT(ANreadann(sel_fd, trunc_desc_buf.data(), 4), Eq(SUCCEED));

  // Query ANid2tagref for file description
  uint16 fd_tag = 0, fd_ref = 0;
  EXPECT_THAT(ANid2tagref(sel_fd, &fd_tag, &fd_ref), Eq(SUCCEED));
  EXPECT_THAT(fd_tag, Eq(DFTAG_FD));
  EXPECT_THAT(fd_ref, Gt(0));
  EXPECT_THAT(ANendaccess(sel_fd), Eq(SUCCEED));

  // Test ANget_tagref for AN_FILE_DESC
  uint16 get_fd_tag = 0, get_fd_ref = 0;
  EXPECT_THAT(ANget_tagref(ran_id, 0, AN_FILE_DESC, &get_fd_tag, &get_fd_ref),
              Eq(SUCCEED));
  EXPECT_THAT(get_fd_tag, Eq(DFTAG_FD));
  EXPECT_THAT(get_fd_ref, Eq(fd_ref));

  // Test ANget_tagref with index == an_num[type] (hits tbbtindx NULL branch due
  // to <= check)
  EXPECT_THAT(ANget_tagref(ran_id, 1, AN_FILE_DESC, &get_fd_tag, &get_fd_ref),
              Eq(FAIL));

  // Test ANtagref2id for DFTAG_FD
  EXPECT_THAT(ANtagref2id(ran_id, DFTAG_FD, fd_ref), Ne(FAIL));

  EXPECT_THAT(ANend(ran_id), Eq(SUCCEED));
  EXPECT_THAT(Hclose(rfid), Eq(SUCCEED));
}

TEST(MfanTest, DataAnnotationsLifecycleAndPersistence) {
  const std::string path = GetTempFilePath("mfan_data_ann.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));

  // Create two distinct data elements in the file
  const uint16 kTargetTag = 700;
  const uint16 kTargetRef1 = 1;
  const uint16 kTargetRef2 = 2;
  const uint8_t dummy_data[4] = {1, 2, 3, 4};

  const int32_t aid1 = Hstartwrite(fid, kTargetTag, kTargetRef1, 4);
  ASSERT_THAT(aid1, Ne(FAIL));
  EXPECT_THAT(Hwrite(aid1, 4, dummy_data), Eq(4));
  EXPECT_THAT(Hendaccess(aid1), Eq(SUCCEED));

  const int32_t aid2 = Hstartwrite(fid, kTargetTag, kTargetRef2, 4);
  ASSERT_THAT(aid2, Ne(FAIL));
  EXPECT_THAT(Hwrite(aid2, 4, dummy_data), Eq(4));
  EXPECT_THAT(Hendaccess(aid2), Eq(SUCCEED));

  const int32_t an_id = ANstart(fid);
  ASSERT_THAT(an_id, Ne(FAIL));

  // 1. Create two data labels and one description for target (700, 1)
  const int32_t dl1_id =
      ANcreate(an_id, kTargetTag, kTargetRef1, AN_DATA_LABEL);
  ASSERT_THAT(dl1_id, Ne(FAIL));
  constexpr std::string_view kDataLabel1 = "First Label for Element 1";
  EXPECT_THAT(ANwriteann(dl1_id, kDataLabel1.data(),
                         static_cast<int32>(kDataLabel1.size())),
              Eq(SUCCEED));

  const int32_t dl2_id =
      ANcreate(an_id, kTargetTag, kTargetRef1, AN_DATA_LABEL);
  ASSERT_THAT(dl2_id, Ne(FAIL));
  constexpr std::string_view kDataLabel2 = "Second Label for Element 1";
  EXPECT_THAT(ANwriteann(dl2_id, kDataLabel2.data(),
                         static_cast<int32>(kDataLabel2.size())),
              Eq(SUCCEED));

  const int32_t dd1_id = ANcreate(an_id, kTargetTag, kTargetRef1, AN_DATA_DESC);
  ASSERT_THAT(dd1_id, Ne(FAIL));
  constexpr std::string_view kDataDesc1 = "Description for Target 1";
  EXPECT_THAT(ANwriteann(dd1_id, kDataDesc1.data(),
                         static_cast<int32>(kDataDesc1.size())),
              Eq(SUCCEED));

  // 2. Create one label for target (700, 2)
  const int32_t dl3_id =
      ANcreate(an_id, kTargetTag, kTargetRef2, AN_DATA_LABEL);
  ASSERT_THAT(dl3_id, Ne(FAIL));
  constexpr std::string_view kDataLabel3 = "Label for Element 2";
  EXPECT_THAT(ANwriteann(dl3_id, kDataLabel3.data(),
                         static_cast<int32>(kDataLabel3.size())),
              Eq(SUCCEED));

  // Test ANnumann
  EXPECT_THAT(ANnumann(an_id, AN_DATA_LABEL, kTargetTag, kTargetRef1), Eq(2));
  EXPECT_THAT(ANnumann(an_id, AN_DATA_DESC, kTargetTag, kTargetRef1), Eq(1));
  EXPECT_THAT(ANnumann(an_id, AN_DATA_LABEL, kTargetTag, kTargetRef2), Eq(1));
  EXPECT_THAT(ANnumann(an_id, AN_DATA_DESC, kTargetTag, kTargetRef2), Eq(0));
  EXPECT_THAT(ANnumann(an_id, AN_DATA_LABEL, 9999, 9999), Eq(0));

  // Test ANannlist
  int32 ann_list[5] = {0};
  EXPECT_THAT(
      ANannlist(an_id, AN_DATA_LABEL, kTargetTag, kTargetRef1, ann_list),
      Eq(2));
  EXPECT_THAT(ann_list[0], Eq(dl2_id));
  EXPECT_THAT(ann_list[1], Eq(dl1_id));

  // Test rewriting an existing data label (newflag == 0)
  constexpr std::string_view kUpdatedLabel1 = "Updated Data Label 1";
  EXPECT_THAT(ANwriteann(dl1_id, kUpdatedLabel1.data(),
                         static_cast<int32>(kUpdatedLabel1.size())),
              Eq(SUCCEED));
  EXPECT_THAT(ANannlen(dl1_id), Eq(static_cast<int32>(kUpdatedLabel1.size())));

  // End all accesses
  EXPECT_THAT(ANendaccess(dl1_id), Eq(SUCCEED));
  EXPECT_THAT(ANendaccess(dl2_id), Eq(SUCCEED));
  EXPECT_THAT(ANendaccess(dd1_id), Eq(SUCCEED));
  EXPECT_THAT(ANendaccess(dl3_id), Eq(SUCCEED));

  EXPECT_THAT(ANend(an_id), Eq(SUCCEED));
  EXPECT_THAT(Hclose(fid), Eq(SUCCEED));

  // 3. Re-open and verify data annotations read back properly
  const int32_t rfid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_THAT(rfid, Ne(FAIL));
  const int32_t ran_id = ANstart(rfid);
  ASSERT_THAT(ran_id, Ne(FAIL));

  int32 n_fl = 0, n_fd = 0, n_dl = 0, n_dd = 0;
  EXPECT_THAT(ANfileinfo(ran_id, &n_fl, &n_fd, &n_dl, &n_dd), Eq(SUCCEED));
  EXPECT_THAT(n_fl, Eq(0));
  EXPECT_THAT(n_fd, Eq(0));
  EXPECT_THAT(n_dl, Eq(3));
  EXPECT_THAT(n_dd, Eq(1));

  EXPECT_THAT(ANnumann(ran_id, AN_DATA_LABEL, kTargetTag, kTargetRef1), Eq(2));
  EXPECT_THAT(ANnumann(ran_id, AN_DATA_DESC, kTargetTag, kTargetRef1), Eq(1));

  // Read through select by index
  for (int32 i = 0; i < n_dl; ++i) {
    const int32_t sel = ANselect(ran_id, i, AN_DATA_LABEL);
    ASSERT_THAT(sel, Ne(FAIL));
    const int32_t len = ANannlen(sel);
    EXPECT_THAT(len, Gt(0));
    std::vector<char> buf(len + 1, '\0');
    EXPECT_THAT(ANreadann(sel, buf.data(), static_cast<int32>(buf.size())),
                Eq(SUCCEED));
    EXPECT_THAT(ANendaccess(sel), Eq(SUCCEED));
  }

  // Select and read back data description
  const int32_t sel_dd = ANselect(ran_id, 0, AN_DATA_DESC);
  ASSERT_THAT(sel_dd, Ne(FAIL));
  const int32_t dd_len = ANannlen(sel_dd);
  EXPECT_THAT(dd_len, Gt(0));
  std::vector<char> dd_buf(dd_len + 1, '\0');
  EXPECT_THAT(
      ANreadann(sel_dd, dd_buf.data(), static_cast<int32>(dd_buf.size())),
      Eq(SUCCEED));
  EXPECT_THAT(std::string_view(dd_buf.data()), Eq(kDataDesc1));

  // Truncated read on data description
  std::vector<char> short_dd_buf(5, '\0');
  EXPECT_THAT(ANreadann(sel_dd, short_dd_buf.data(), 4), Eq(SUCCEED));

  // Query ANid2tagref for data description
  uint16 dd_tag = 0, dd_ref = 0;
  EXPECT_THAT(ANid2tagref(sel_dd, &dd_tag, &dd_ref), Eq(SUCCEED));
  EXPECT_THAT(dd_tag, Eq(DFTAG_DIA));
  EXPECT_THAT(dd_ref, Gt(0));
  EXPECT_THAT(ANendaccess(sel_dd), Eq(SUCCEED));

  // Query ANid2tagref for data label
  const int32_t sel_dl0 = ANselect(ran_id, 0, AN_DATA_LABEL);
  ASSERT_THAT(sel_dl0, Ne(FAIL));
  uint16 id2_dl_tag = 0, id2_dl_ref = 0;
  EXPECT_THAT(ANid2tagref(sel_dl0, &id2_dl_tag, &id2_dl_ref), Eq(SUCCEED));
  EXPECT_THAT(id2_dl_tag, Eq(DFTAG_DIL));
  EXPECT_THAT(id2_dl_ref, Gt(0));
  EXPECT_THAT(ANendaccess(sel_dl0), Eq(SUCCEED));

  // Query ANget_tagref for AN_DATA_LABEL and AN_DATA_DESC
  uint16 dl_tag = 0, dl_ref = 0;
  EXPECT_THAT(ANget_tagref(ran_id, 0, AN_DATA_LABEL, &dl_tag, &dl_ref),
              Eq(SUCCEED));
  EXPECT_THAT(dl_tag, Eq(DFTAG_DIL));
  EXPECT_THAT(dl_ref, Gt(0));

  uint16 get_dd_tag = 0, get_dd_ref = 0;
  EXPECT_THAT(ANget_tagref(ran_id, 0, AN_DATA_DESC, &get_dd_tag, &get_dd_ref),
              Eq(SUCCEED));
  EXPECT_THAT(get_dd_tag, Eq(DFTAG_DIA));
  EXPECT_THAT(get_dd_ref, Eq(dd_ref));

  // Query ANtagref2id for DFTAG_DIL and DFTAG_DIA
  EXPECT_THAT(ANtagref2id(ran_id, DFTAG_DIL, dl_ref), Ne(FAIL));
  EXPECT_THAT(ANtagref2id(ran_id, DFTAG_DIA, dd_ref), Ne(FAIL));

  EXPECT_THAT(ANend(ran_id), Eq(SUCCEED));
  EXPECT_THAT(Hclose(rfid), Eq(SUCCEED));
}

TEST(MfanTest, InvalidArgumentsAndErrorBranches) {
  const std::string path = GetTempFilePath("mfan_errors.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t an_id = ANstart(fid);
  ASSERT_THAT(an_id, Ne(FAIL));
  absl::Cleanup close_an = [an_id] { EXPECT_THAT(ANend(an_id), Eq(SUCCEED)); };

  // 1. ANcreate with invalid parameters
  EXPECT_THAT(ANcreate(FAIL, 100, 1, AN_DATA_LABEL), Eq(FAIL));
  // Invalid types for ANcreate (must be DATA_LABEL or DATA_DESC)
  EXPECT_THAT(ANcreate(an_id, 100, 1, static_cast<ann_type>(999)), Eq(FAIL));
  // Invalid elem_tag (0) or elem_ref (0)
  EXPECT_THAT(ANcreate(an_id, 0, 1, AN_DATA_LABEL), Eq(FAIL));
  EXPECT_THAT(ANcreate(an_id, 100, 0, AN_DATA_LABEL), Eq(FAIL));

  // 2. ANcreatef with invalid parameters
  EXPECT_THAT(ANcreatef(FAIL, AN_FILE_LABEL), Eq(FAIL));
  // Invalid types for ANcreatef (must be FILE_LABEL or FILE_DESC)
  EXPECT_THAT(ANcreatef(an_id, AN_DATA_LABEL), Eq(FAIL));
  EXPECT_THAT(ANcreatef(an_id, AN_DATA_DESC), Eq(FAIL));
  EXPECT_THAT(ANcreatef(an_id, static_cast<ann_type>(999)), Eq(FAIL));

  // 3. ANselect with invalid parameters
  EXPECT_THAT(ANselect(FAIL, 0, AN_FILE_LABEL), Eq(FAIL));
  EXPECT_THAT(ANselect(an_id, -1, AN_FILE_LABEL), Eq(FAIL));
  EXPECT_THAT(ANselect(an_id, 9999, AN_FILE_LABEL), Eq(FAIL));

  // 4. ANnumann with invalid parameters
  EXPECT_THAT(ANnumann(FAIL, AN_DATA_LABEL, 100, 1), Eq(FAIL));
  EXPECT_THAT(ANnumann(an_id, AN_FILE_LABEL, 100, 1), Eq(FAIL));
  EXPECT_THAT(ANnumann(an_id, AN_FILE_DESC, 100, 1), Eq(FAIL));

  // 5. ANannlist with invalid parameters
  int32 list[5];
  EXPECT_THAT(ANannlist(FAIL, AN_DATA_LABEL, 100, 1, list), Eq(FAIL));
  EXPECT_THAT(ANannlist(an_id, AN_FILE_LABEL, 100, 1, list), Eq(FAIL));
  EXPECT_THAT(ANannlist(an_id, AN_FILE_DESC, 100, 1, list), Eq(FAIL));

  // 6. ANannlen with invalid parameters
  EXPECT_THAT(ANannlen(FAIL), Eq(FAIL));

  // 7. ANwriteann with invalid parameters
  EXPECT_THAT(ANwriteann(FAIL, "test", 4), Eq(FAIL));
  const int32_t valid_ann = ANcreatef(an_id, AN_FILE_LABEL);
  ASSERT_THAT(valid_ann, Ne(FAIL));
  absl::Cleanup close_ann_handle = [valid_ann] {
    EXPECT_THAT(ANendaccess(valid_ann), Eq(SUCCEED));
  };
  EXPECT_THAT(ANwriteann(valid_ann, nullptr, 4), Eq(FAIL));
  EXPECT_THAT(ANwriteann(valid_ann, "test", 0), Eq(FAIL));
  EXPECT_THAT(ANwriteann(valid_ann, "test", -1), Eq(FAIL));

  // 8. ANreadann with invalid parameters
  char buf[32];
  EXPECT_THAT(ANreadann(FAIL, buf, sizeof(buf)), Eq(FAIL));
  EXPECT_THAT(ANreadann(valid_ann, nullptr, sizeof(buf)), Eq(FAIL));
  EXPECT_THAT(ANreadann(valid_ann, buf, 0), Eq(FAIL));
  EXPECT_THAT(ANreadann(valid_ann, buf, -1), Eq(FAIL));

  // Reading or querying length of unwritten annotation should fail
  EXPECT_THAT(ANannlen(valid_ann), Eq(FAIL));
  EXPECT_THAT(ANreadann(valid_ann, buf, sizeof(buf)), Eq(FAIL));

  const int32_t unwritten_dl = ANcreate(an_id, 100, 1, AN_DATA_LABEL);
  ASSERT_THAT(unwritten_dl, Ne(FAIL));
  absl::Cleanup close_unwritten_dl = [unwritten_dl] {
    EXPECT_THAT(ANendaccess(unwritten_dl), Eq(SUCCEED));
  };
  EXPECT_THAT(ANannlen(unwritten_dl), Eq(FAIL));
  EXPECT_THAT(ANreadann(unwritten_dl, buf, sizeof(buf)), Eq(FAIL));

  // 9. ANendaccess is currently a no-op that always returns SUCCEED
  EXPECT_THAT(ANendaccess(FAIL), Eq(SUCCEED));

  // 10. ANget_tagref with invalid parameters
  uint16 tag = 0, ref = 0;
  EXPECT_THAT(ANget_tagref(FAIL, 0, AN_FILE_LABEL, &tag, &ref), Eq(FAIL));
  EXPECT_THAT(ANget_tagref(an_id, -1, AN_FILE_LABEL, &tag, &ref), Eq(FAIL));
  EXPECT_THAT(ANget_tagref(an_id, 9999, AN_FILE_LABEL, &tag, &ref), Eq(FAIL));

  // 11. ANid2tagref with invalid parameters
  EXPECT_THAT(ANid2tagref(FAIL, &tag, &ref), Eq(FAIL));

  // 12. ANtagref2id with invalid parameters
  EXPECT_THAT(ANtagref2id(FAIL, DFTAG_FID, 1), Eq(FAIL));
  EXPECT_THAT(ANtagref2id(an_id, DFTAG_NULL, 1), Eq(FAIL));
  EXPECT_THAT(ANtagref2id(an_id, DFTAG_FID, DFREF_NONE), Eq(FAIL));
  EXPECT_THAT(ANtagref2id(an_id, DFTAG_FID, 9999), Eq(FAIL));
}

TEST(MfanTest, UninitializedTreesLazyCreation) {
  const std::string path = GetTempFilePath("mfan_uninit.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t an_id = ANstart(fid);
  ASSERT_THAT(an_id, Ne(FAIL));
  absl::Cleanup close_an = [an_id] { EXPECT_THAT(ANend(an_id), Eq(SUCCEED)); };

  // Call ANnumann on fresh file when trees are uninitialized (an_num == -1)
  EXPECT_THAT(ANnumann(an_id, AN_DATA_LABEL, 100, 1), Eq(0));
  // Call ANselect on fresh file when trees are uninitialized (an_num == -1)
  EXPECT_THAT(ANselect(an_id, 0, AN_FILE_LABEL), Eq(FAIL));
  // Call ANget_tagref on fresh file when trees are uninitialized (an_num == -1)
  uint16 tag = 0, ref = 0;
  EXPECT_THAT(ANget_tagref(an_id, 0, AN_DATA_DESC, &tag, &ref), Eq(FAIL));
  // Call ANtagref2id on fresh file when trees are uninitialized (an_num == -1)
  EXPECT_THAT(ANtagref2id(an_id, DFTAG_FD, 1), Eq(FAIL));

  // Call ANannlist on fresh file where tree is uninitialized
  const std::string path2 = GetTempFilePath("mfan_annlist_uninit.hdf");
  std::filesystem::remove(path2);
  absl::Cleanup remove_temp2 = [&path2] { std::filesystem::remove(path2); };
  const int32_t fid2 = Hopen(path2.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid2, Ne(FAIL));
  const int32_t an_id2 = ANstart(fid2);
  ASSERT_THAT(an_id2, Ne(FAIL));
  int32 ann_list[5];
  EXPECT_THAT(ANannlist(an_id2, AN_DATA_LABEL, 100, 1, ann_list), Eq(0));
  EXPECT_THAT(ANend(an_id2), Eq(SUCCEED));
  EXPECT_THAT(Hclose(fid2), Eq(SUCCEED));
}

TEST(MfanTest, AnnListLoadsAnnotationsFromDisk) {
  const std::string path = GetTempFilePath("mfan_annlist.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t an_id = ANstart(fid);
    ASSERT_THAT(an_id, Ne(FAIL));

    // Create target data element
    const int32_t aid = Hstartwrite(fid, 800, 1, 10);
    ASSERT_THAT(aid, Ne(FAIL));
    EXPECT_THAT(Hwrite(aid, 4, "data"), Eq(4));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

    const int32_t ann1 = ANcreate(an_id, 800, 1, AN_DATA_LABEL);
    ASSERT_THAT(ann1, Ne(FAIL));
    EXPECT_THAT(ANwriteann(ann1, "Label1", 6), Eq(SUCCEED));
    EXPECT_THAT(ANendaccess(ann1), Eq(SUCCEED));

    const int32_t ann2 = ANcreate(an_id, 800, 1, AN_DATA_DESC);
    ASSERT_THAT(ann2, Ne(FAIL));
    EXPECT_THAT(ANwriteann(ann2, "Desc1", 5), Eq(SUCCEED));
    EXPECT_THAT(ANendaccess(ann2), Eq(SUCCEED));

    EXPECT_THAT(ANend(an_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Reopen and call ANannlist without calling ANfileinfo first
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t an_id = ANstart(fid);
    ASSERT_THAT(an_id, Ne(FAIL));

    int32 list[5] = {0};
    EXPECT_THAT(ANannlist(an_id, AN_DATA_LABEL, 800, 1, list), Eq(1));
    EXPECT_THAT(list[0], Ne(0));

    int32 list_desc[5] = {0};
    EXPECT_THAT(ANannlist(an_id, AN_DATA_DESC, 800, 1, list_desc), Eq(1));
    EXPECT_THAT(list_desc[0], Ne(0));

    EXPECT_THAT(ANend(an_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

extern "C" {
void ANfreedata(void* data);
void ANfreekey(void* key);
int ANIanncmp(void* i, void* j, int value);
}

TEST(MfanTest, DirectTbbtHelpers) {
  // Test ANfreedata and ANfreekey on dynamically allocated pointers
  void* data = malloc(16);
  ASSERT_THAT(data, NotNull());
  ANfreedata(data);

  void* key = malloc(8);
  ASSERT_THAT(key, NotNull());
  ANfreekey(key);

  // ANIanncmp takes two int32 pointers
  int32 k1 = AN_CREATE_KEY(AN_DATA_LABEL, 1);
  int32 k2 = AN_CREATE_KEY(AN_DATA_LABEL, 2);
  int32 k3 = AN_CREATE_KEY(AN_DATA_DESC, 1);
  int32 k4 = AN_CREATE_KEY(AN_DATA_LABEL, 1);

  EXPECT_THAT(ANIanncmp(&k1, &k4, 0), Eq(0));
  EXPECT_THAT(ANIanncmp(&k1, &k2, 0), Gt(0));
  EXPECT_THAT(ANIanncmp(&k2, &k1, 0), Lt(0));
  EXPECT_THAT(ANIanncmp(&k1, &k3, 0), Ne(0));
}

TEST(MfanTest, TruncatedFileDataAnnotationFailsReading) {
  const std::string path = GetTempFilePath("mfan_truncated.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  // 1. Create a valid file with a data label element
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t aid = Hstartwrite(fid, DFTAG_DIL, 1, 100);
    ASSERT_THAT(aid, Ne(FAIL));
    char payload[100] = {0};
    EXPECT_THAT(Hwrite(aid, sizeof(payload), payload), Eq(sizeof(payload)));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // 2. Truncate the file so that the DD block is intact but payload cannot be
  // read
  const auto original_size = std::filesystem::file_size(path);
  ASSERT_THAT(original_size, Gt(100));
  // Truncate to before the payload starts
  std::filesystem::resize_file(path, original_size - 100);

  // 3. Open the file and call ANfileinfo
  const int32_t rfid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_THAT(rfid, Ne(FAIL));
  const int32_t ran_id = ANstart(rfid);
  ASSERT_THAT(ran_id, Ne(FAIL));

  int32 n_fl = 0, n_fd = 0, n_dl = 0, n_dd = 0;
  // ANfileinfo fails when Hread on truncated data label element fails
  EXPECT_THAT(ANfileinfo(ran_id, &n_fl, &n_fd, &n_dl, &n_dd), Eq(FAIL));

  EXPECT_THAT(ANend(ran_id), Eq(SUCCEED));
  EXPECT_THAT(Hclose(rfid), Eq(SUCCEED));

  // 4. Repeat with truncated data description (DFTAG_DIA)
  const std::string path_desc = GetTempFilePath("mfan_truncated_desc.hdf");
  std::filesystem::remove(path_desc);
  absl::Cleanup remove_temp_desc = [&path_desc] {
    std::filesystem::remove(path_desc);
  };
  {
    const int32_t fid = Hopen(path_desc.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t aid = Hstartwrite(fid, DFTAG_DIA, 1, 100);
    ASSERT_THAT(aid, Ne(FAIL));
    char payload[100] = {0};
    EXPECT_THAT(Hwrite(aid, sizeof(payload), payload), Eq(sizeof(payload)));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
  const auto desc_orig_size = std::filesystem::file_size(path_desc);
  ASSERT_THAT(desc_orig_size, Gt(100));
  std::filesystem::resize_file(path_desc, desc_orig_size - 100);

  const int32_t rfid_desc = Hopen(path_desc.c_str(), DFACC_READ, 0);
  ASSERT_THAT(rfid_desc, Ne(FAIL));
  const int32_t ran_desc = ANstart(rfid_desc);
  ASSERT_THAT(ran_desc, Ne(FAIL));

  // ANfileinfo fails when reading data desc, hitting line 1222
  EXPECT_THAT(ANfileinfo(ran_desc, &n_fl, &n_fd, &n_dl, &n_dd), Eq(FAIL));

  EXPECT_THAT(ANend(ran_desc), Eq(SUCCEED));
  EXPECT_THAT(Hclose(rfid_desc), Eq(SUCCEED));
}

}  // namespace
}  // namespace hdf4
