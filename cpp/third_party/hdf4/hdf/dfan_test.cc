// Test author: schwehr@google.com

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/dfan_priv.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/hproto.h"

namespace hdf4 {
namespace {

class DfanTest : public ::testing::Test {
 protected:
  void SetUp() override { DFANclear(); }

  void TearDown() override { DFANclear(); }
};

TEST_F(DfanTest, InvalidArguments) {
  char buf[64] = {};
  uint16_t reflist[10] = {};

  char test_str[] = "test";

  // Null filename
  EXPECT_EQ(DFANgetlablen(nullptr, 100, 1), FAIL);
  EXPECT_EQ(DFANgetlabel(nullptr, 100, 1, buf, sizeof(buf)), FAIL);
  EXPECT_EQ(DFANgetdesclen(nullptr, 100, 1), FAIL);
  EXPECT_EQ(DFANgetdesc(nullptr, 100, 1, buf, sizeof(buf)), FAIL);
  EXPECT_EQ(DFANputlabel(nullptr, 100, 1, test_str), FAIL);
  EXPECT_EQ(DFANputdesc(nullptr, 100, 1, test_str, 4), FAIL);
  EXPECT_EQ(DFANlablist(nullptr, 100, reflist, buf, 10, sizeof(buf), 1), FAIL);
  EXPECT_EQ(DFANIgetannlen(nullptr, 100, 1, DFAN_LABEL), FAIL);
  EXPECT_EQ(DFANIgetann(nullptr, 100, 1, reinterpret_cast<uint8_t*>(buf),
                        sizeof(buf), DFAN_LABEL, 0),
            FAIL);
  EXPECT_EQ(DFANIputann(nullptr, 100, 1, reinterpret_cast<uint8_t*>(buf), 4,
                        DFAN_LABEL),
            FAIL);

  // Non-existent file
  const std::string bad_file = "/non_existent_dir/bad_file.hdf";
  EXPECT_EQ(DFANgetlablen(bad_file.c_str(), 100, 1), FAIL);
  EXPECT_EQ(DFANgetlabel(bad_file.c_str(), 100, 1, buf, sizeof(buf)), FAIL);
  EXPECT_EQ(DFANgetdesclen(bad_file.c_str(), 100, 1), FAIL);
  EXPECT_EQ(DFANgetdesc(bad_file.c_str(), 100, 1, buf, sizeof(buf)), FAIL);
  EXPECT_EQ(DFANputlabel(bad_file.c_str(), 100, 1, test_str), FAIL);
  EXPECT_EQ(DFANputdesc(bad_file.c_str(), 100, 1, test_str, 4), FAIL);
  EXPECT_EQ(
      DFANlablist(bad_file.c_str(), 100, reflist, buf, 10, sizeof(buf), 1),
      FAIL);

  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfan_invalid_args.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);

  // Tag == 0 or Ref == 0
  EXPECT_EQ(DFANgetlablen(path.c_str(), 0, 1), FAIL);
  EXPECT_EQ(DFANgetlablen(path.c_str(), 100, 0), FAIL);
  EXPECT_EQ(DFANgetlabel(path.c_str(), 0, 1, buf, sizeof(buf)), FAIL);
  EXPECT_EQ(DFANgetlabel(path.c_str(), 100, 0, buf, sizeof(buf)), FAIL);
  EXPECT_EQ(DFANgetlabel(path.c_str(), 100, 1, nullptr, sizeof(buf)), FAIL);

  EXPECT_EQ(DFANgetdesclen(path.c_str(), 0, 1), FAIL);
  EXPECT_EQ(DFANgetdesclen(path.c_str(), 100, 0), FAIL);
  EXPECT_EQ(DFANgetdesc(path.c_str(), 0, 1, buf, sizeof(buf)), FAIL);
  EXPECT_EQ(DFANgetdesc(path.c_str(), 100, 0, buf, sizeof(buf)), FAIL);
  EXPECT_EQ(DFANgetdesc(path.c_str(), 100, 1, nullptr, sizeof(buf)), FAIL);

  EXPECT_EQ(DFANputlabel(path.c_str(), 0, 1, test_str), FAIL);
  EXPECT_EQ(DFANputlabel(path.c_str(), 100, 0, test_str), FAIL);
  EXPECT_EQ(DFANputlabel(path.c_str(), 100, 1, nullptr), FAIL);

  EXPECT_EQ(DFANputdesc(path.c_str(), 0, 1, test_str, 4), FAIL);
  EXPECT_EQ(DFANputdesc(path.c_str(), 100, 0, test_str, 4), FAIL);
  EXPECT_EQ(DFANputdesc(path.c_str(), 100, 1, nullptr, 4), FAIL);
  EXPECT_EQ(DFANputdesc(path.c_str(), 100, 1, test_str, -5), FAIL);

  // Lablist bad args
  EXPECT_EQ(DFANlablist(path.c_str(), 0, reflist, buf, 10, sizeof(buf), 1),
            FAIL);
  EXPECT_EQ(DFANlablist(path.c_str(), 100, nullptr, buf, 10, sizeof(buf), 1),
            FAIL);
  EXPECT_EQ(
      DFANlablist(path.c_str(), 100, reflist, nullptr, 10, sizeof(buf), 1),
      FAIL);

  // File annotation bad args
  EXPECT_EQ(DFANgetfidlen(FAIL, 1), FAIL);
  EXPECT_EQ(DFANgetfid(FAIL, buf, sizeof(buf), 1), FAIL);
  EXPECT_EQ(DFANgetfid(fid, nullptr, sizeof(buf), 1), FAIL);
  EXPECT_EQ(DFANgetfdslen(FAIL, 1), FAIL);
  EXPECT_EQ(DFANgetfds(FAIL, buf, sizeof(buf), 1), FAIL);
  EXPECT_EQ(DFANgetfds(fid, nullptr, sizeof(buf), 1), FAIL);
  EXPECT_EQ(DFANaddfid(FAIL, const_cast<char*>("test")), FAIL);
  EXPECT_EQ(DFANaddfid(fid, nullptr), FAIL);
  EXPECT_EQ(DFANaddfds(FAIL, const_cast<char*>("test"), 4), FAIL);
  EXPECT_EQ(DFANaddfds(fid, nullptr, 4), FAIL);

  // DFANI locate bad fid
  EXPECT_EQ(DFANIlocate(FAIL, DFAN_LABEL, 100, 1), 0);
  EXPECT_EQ(DFANIlocate(fid, DFAN_LABEL, 0, 0), 0);  // 0 anns -> FAIL (0)

  ASSERT_NE(Hclose(fid), FAIL);
}

TEST_F(DfanTest, PutAndGetLabelsAndDescriptions) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfan_labels_descs.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  // Create an empty HDF file
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);
  }

  // Reading non-existent label and description
  EXPECT_EQ(DFANgetlablen(path.c_str(), 100, 1), FAIL);
  char buf[128] = {};
  EXPECT_EQ(DFANgetlabel(path.c_str(), 100, 1, buf, sizeof(buf)), FAIL);
  EXPECT_EQ(DFANgetdesclen(path.c_str(), 100, 1), FAIL);
  EXPECT_EQ(DFANgetdesc(path.c_str(), 100, 1, buf, sizeof(buf)), FAIL);

  // Put a label on object (tag 200, ref 5)
  EXPECT_EQ(
      DFANputlabel(path.c_str(), 200, 5, const_cast<char*>("My First Label")),
      SUCCEED);
  EXPECT_NE(DFANlastref(), 0);
  const uint16_t label_ref = DFANlastref();

  // Get label length
  const int32_t lablen = DFANgetlablen(path.c_str(), 200, 5);
  EXPECT_EQ(lablen, static_cast<int32_t>(strlen("My First Label")));
  EXPECT_EQ(DFANlastref(), label_ref);

  // Get label content
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(DFANgetlabel(path.c_str(), 200, 5, buf, sizeof(buf)), SUCCEED);
  EXPECT_STREQ(buf, "My First Label");

  // Get label with truncation (maxlen smaller than label + 1)
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(DFANgetlabel(path.c_str(), 200, 5, buf, 6), SUCCEED);
  EXPECT_STREQ(buf, "My Fi");  // 5 chars + null

  // Put a description on object (tag 200, ref 5)
  const std::string desc_text = "Detailed description of object 200/5.";
  EXPECT_EQ(DFANputdesc(path.c_str(), 200, 5,
                        const_cast<char*>(desc_text.c_str()), desc_text.size()),
            SUCCEED);
  const uint16_t desc_ref = DFANlastref();
  EXPECT_NE(desc_ref, 0);

  // Get description length
  const int32_t desclen = DFANgetdesclen(path.c_str(), 200, 5);
  EXPECT_EQ(desclen, static_cast<int32_t>(desc_text.size()));
  EXPECT_EQ(DFANlastref(), desc_ref);

  // Get description content
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(DFANgetdesc(path.c_str(), 200, 5, buf, sizeof(buf)), SUCCEED);
  buf[desclen] = '\0';
  EXPECT_STREQ(buf, desc_text.c_str());

  // Get description with truncation
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(DFANgetdesc(path.c_str(), 200, 5, buf, 8), SUCCEED);
  buf[8] = '\0';
  EXPECT_STREQ(buf, "Detailed");

  // Overwrite existing label on object (tag 200, ref 5)
  EXPECT_EQ(
      DFANputlabel(path.c_str(), 200, 5, const_cast<char*>("Updated Label")),
      SUCCEED);
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(DFANgetlabel(path.c_str(), 200, 5, buf, sizeof(buf)), SUCCEED);
  EXPECT_STREQ(buf, "Updated Label");

  // Overwrite existing description on object (tag 200, ref 5)
  const std::string updated_desc = "New description.";
  EXPECT_EQ(
      DFANputdesc(path.c_str(), 200, 5, const_cast<char*>(updated_desc.c_str()),
                  updated_desc.size()),
      SUCCEED);
  memset(buf, 0, sizeof(buf));
  const int32_t new_desclen = DFANgetdesclen(path.c_str(), 200, 5);
  EXPECT_EQ(new_desclen, static_cast<int32_t>(updated_desc.size()));
  EXPECT_EQ(DFANgetdesc(path.c_str(), 200, 5, buf, sizeof(buf)), SUCCEED);
  buf[new_desclen] = '\0';
  EXPECT_STREQ(buf, updated_desc.c_str());

  // Clear in-memory directory so getting label and desc must rescan file
  DFANclear();
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(DFANgetlabel(path.c_str(), 200, 5, buf, sizeof(buf)), SUCCEED);
  EXPECT_STREQ(buf, "Updated Label");

  DFANclear();
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(DFANgetdesc(path.c_str(), 200, 5, buf, sizeof(buf)), SUCCEED);
  buf[new_desclen] = '\0';
  EXPECT_STREQ(buf, updated_desc.c_str());
}

TEST_F(DfanTest, FileAnnotations) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfan_file_anns.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);

  // Before adding any file annotations, reading returns FAIL
  char buf[128] = {};
  EXPECT_EQ(DFANgetfidlen(fid, 1), FAIL);
  EXPECT_EQ(DFANgetfid(fid, buf, sizeof(buf), 1), FAIL);
  EXPECT_EQ(DFANgetfdslen(fid, 1), FAIL);
  EXPECT_EQ(DFANgetfds(fid, buf, sizeof(buf), 1), FAIL);

  // Add multiple file IDs
  EXPECT_EQ(DFANaddfid(fid, const_cast<char*>("File ID 1")), SUCCEED);
  const uint16_t fid_ref1 = DFANlastref();
  EXPECT_NE(fid_ref1, 0);

  EXPECT_EQ(DFANaddfid(fid, const_cast<char*>("File ID 2 (longer)")), SUCCEED);
  const uint16_t fid_ref2 = DFANlastref();
  EXPECT_NE(fid_ref2, 0);

  // Read first file ID length and content
  int32_t len1 = DFANgetfidlen(fid, 1);
  EXPECT_EQ(len1, static_cast<int32_t>(strlen("File ID 1")));
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(DFANgetfid(fid, buf, sizeof(buf), 1), len1);
  EXPECT_STREQ(buf, "File ID 1");

  // Read next file ID length and content
  int32_t len2 = DFANgetfidlen(fid, 0);
  EXPECT_EQ(len2, static_cast<int32_t>(strlen("File ID 2 (longer)")));
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(DFANgetfid(fid, buf, sizeof(buf), 0), len2);
  EXPECT_STREQ(buf, "File ID 2 (longer)");

  // Read past last file ID -> FAIL
  EXPECT_EQ(DFANgetfidlen(fid, 0), FAIL);
  EXPECT_EQ(DFANgetfid(fid, buf, sizeof(buf), 0), FAIL);

  // Truncated file ID read
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(DFANgetfid(fid, buf, 5, 1), 4);
  EXPECT_STREQ(buf, "File");

  // Add multiple file descriptions
  const std::string fds1 = "First file description.";
  const std::string fds2 = "Second file description, more details.";
  EXPECT_EQ(DFANaddfds(fid, const_cast<char*>(fds1.c_str()), fds1.size()),
            SUCCEED);
  EXPECT_EQ(DFANaddfds(fid, const_cast<char*>(fds2.c_str()), fds2.size()),
            SUCCEED);

  // Read first file description
  int32_t fdslen1 = DFANgetfdslen(fid, 1);
  EXPECT_EQ(fdslen1, static_cast<int32_t>(fds1.size()));
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(DFANgetfds(fid, buf, sizeof(buf), 1), fdslen1);
  EXPECT_STREQ(buf, fds1.c_str());

  // Read next file description
  int32_t fdslen2 = DFANgetfdslen(fid, 0);
  EXPECT_EQ(fdslen2, static_cast<int32_t>(fds2.size()));
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(DFANgetfds(fid, buf, sizeof(buf), 0), fdslen2);
  EXPECT_STREQ(buf, fds2.c_str());

  // Read past last file description -> FAIL
  EXPECT_EQ(DFANgetfdslen(fid, 0), FAIL);
  EXPECT_EQ(DFANgetfds(fid, buf, sizeof(buf), 0), FAIL);

  // Truncated file description read
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(DFANgetfds(fid, buf, 6, 1), 5);
  EXPECT_STREQ(buf, "First");

  ASSERT_NE(Hclose(fid), FAIL);
}

TEST_F(DfanTest, LablistAndFortranModes) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfan_lablist.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr uint16_t target_tag = 300;
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    // Create 3 elements with target_tag
    const uint8_t dummy = 1;
    ASSERT_NE(Hputelement(fid, target_tag, 10, &dummy, 1), FAIL);
    ASSERT_NE(Hputelement(fid, target_tag, 20, &dummy, 1), FAIL);
    ASSERT_NE(Hputelement(fid, target_tag, 30, &dummy, 1), FAIL);
    ASSERT_NE(Hclose(fid), FAIL);
  }

  // Label only ref 10 and ref 30; ref 20 has no label
  EXPECT_EQ(
      DFANputlabel(path.c_str(), target_tag, 10, const_cast<char*>("Label 10")),
      SUCCEED);
  EXPECT_EQ(
      DFANputlabel(path.c_str(), target_tag, 30, const_cast<char*>("Label 30")),
      SUCCEED);

  // Clear directory so DFANlablist must call DFANIlocate to scan labels
  DFANclear();

  // Lablist C mode
  constexpr int listsize = 5;
  constexpr int maxlen = 32;
  uint16_t reflist[listsize] = {};
  char labellist[listsize * maxlen] = {};

  int count = DFANlablist(path.c_str(), target_tag, reflist, labellist,
                          listsize, maxlen, 1);
  EXPECT_EQ(count, 3);
  EXPECT_EQ(reflist[0], 10);
  EXPECT_EQ(reflist[1], 20);
  EXPECT_EQ(reflist[2], 30);
  EXPECT_STREQ(labellist, "Label 10");
  EXPECT_EQ(labellist[1 * maxlen], '\0');  // unlabelled entry is null
  EXPECT_STREQ(labellist + 2 * maxlen, "Label 30");

  // Lablist with startpos = 2
  memset(reflist, 0, sizeof(reflist));
  memset(labellist, 0, sizeof(labellist));
  count = DFANlablist(path.c_str(), target_tag, reflist, labellist, listsize,
                      maxlen, 2);
  EXPECT_EQ(count, 2);
  EXPECT_EQ(reflist[0], 20);
  EXPECT_EQ(reflist[1], 30);
  EXPECT_EQ(labellist[0], '\0');
  EXPECT_STREQ(labellist + 1 * maxlen, "Label 30");

  // Lablist Fortran mode (padded with spaces)
  memset(reflist, 0, sizeof(reflist));
  memset(labellist, 0, sizeof(labellist));
  count = DFANIlablist(path.c_str(), target_tag, reflist,
                       reinterpret_cast<uint8_t*>(labellist), listsize, maxlen,
                       1, 1);
  EXPECT_EQ(count, 3);
  // Ref 20 has no label, so it should be filled with spaces
  for (int i = 0; i < maxlen; ++i) {
    EXPECT_EQ(labellist[1 * maxlen + i], ' ');
  }

  // Fortran mode in DFANIgetann
  char fbuf[32] = {};
  EXPECT_EQ(DFANIgetann(path.c_str(), target_tag, 10,
                        reinterpret_cast<uint8_t*>(fbuf), 4, DFAN_LABEL, 1),
            SUCCEED);
  // Truncated to 4 bytes without null terminator
  EXPECT_EQ(std::string(fbuf, 4), "Labe");

  // Fortran mode for description
  EXPECT_EQ(DFANputdesc(path.c_str(), target_tag, 10,
                        const_cast<char*>("Description 10"), 14),
            SUCCEED);
  memset(fbuf, 0, sizeof(fbuf));
  EXPECT_EQ(DFANIgetann(path.c_str(), target_tag, 10,
                        reinterpret_cast<uint8_t*>(fbuf), 5, DFAN_DESC, 1),
            SUCCEED);
  EXPECT_EQ(std::string(fbuf, 5), "Descr");
}

TEST_F(DfanTest, DirectoryExpansionAndCaching) {
  const std::string dir = ::testing::TempDir();
  const std::string path1 = dir + "/dfan_many1.hdf";
  const std::string path2 = dir + "/dfan_many2.hdf";
  (void)remove(path1.c_str());
  (void)remove(path2.c_str());
  const absl::Cleanup cleanup = [&path1, &path2] {
    (void)remove(path1.c_str());
    (void)remove(path2.c_str());
  };

  {
    const int32_t fid = Hopen(path1.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);
  }

  // Add 25 labels to exceed DFAN_DEFENTRIES (16) and trigger directory block
  // allocation
  for (int i = 1; i <= 25; ++i) {
    const std::string label = "Label " + std::to_string(i);
    EXPECT_EQ(DFANputlabel(path1.c_str(), 500, static_cast<uint16_t>(i),
                           const_cast<char*>(label.c_str())),
              SUCCEED);
  }

  // Read them back from cached file
  for (int i = 1; i <= 25; ++i) {
    char buf[64] = {};
    EXPECT_EQ(DFANgetlabel(path1.c_str(), 500, static_cast<uint16_t>(i), buf,
                           sizeof(buf)),
              SUCCEED);
    const std::string expected = "Label " + std::to_string(i);
    EXPECT_STREQ(buf, expected.c_str());
  }

  // Add 25 descriptions to test description directory expansion
  for (int i = 1; i <= 25; ++i) {
    const std::string desc = "Description text " + std::to_string(i);
    EXPECT_EQ(DFANputdesc(path1.c_str(), 500, static_cast<uint16_t>(i),
                          const_cast<char*>(desc.c_str()), desc.size()),
              SUCCEED);
  }

  for (int i = 1; i <= 25; ++i) {
    char buf[64] = {};
    const int32_t len =
        DFANgetdesclen(path1.c_str(), 500, static_cast<uint16_t>(i));
    const std::string expected = "Description text " + std::to_string(i);
    EXPECT_EQ(len, static_cast<int32_t>(expected.size()));
    EXPECT_EQ(DFANgetdesc(path1.c_str(), 500, static_cast<uint16_t>(i), buf,
                          sizeof(buf)),
              SUCCEED);
    buf[len] = '\0';
    EXPECT_STREQ(buf, expected.c_str());
  }

  // Open path2 to test switching cached file in DFANIopen
  {
    const int32_t fid = Hopen(path2.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);
  }
  EXPECT_EQ(
      DFANputlabel(path2.c_str(), 600, 1, const_cast<char*>("Path2 Label")),
      SUCCEED);
  char buf[64] = {};
  EXPECT_EQ(DFANgetlabel(path2.c_str(), 600, 1, buf, sizeof(buf)), SUCCEED);
  EXPECT_STREQ(buf, "Path2 Label");

  // Re-create path1 with DFACC_CREATE to test cache invalidation on create mode
  {
    const int32_t fid = Hopen(path1.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);
  }
  EXPECT_EQ(DFANgetlablen(path1.c_str(), 500, 1), FAIL);

  // Clear interface explicitly
  EXPECT_EQ(DFANclear(), SUCCEED);
  EXPECT_EQ(DFANIclear(), SUCCEED);
  EXPECT_EQ(DFANPshutdown(), SUCCEED);
}

TEST_F(DfanTest, CorruptedAndEdgeScenarios) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfan_corrupt.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  // 1. Tag exists in file but has 0 elements for DFANIlablist
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    uint16_t reflist[5] = {};
    char labellist[160] = {};
    EXPECT_EQ(DFANlablist(path.c_str(), 777, reflist, labellist, 5, 32, 1),
              FAIL);
  }

  // 2. Put label, then delete its DD so Hstartread in DFANIgetann fails
  {
    EXPECT_EQ(
        DFANputlabel(path.c_str(), 800, 1, const_cast<char*>("Temp Label")),
        SUCCEED);
    const uint16_t annref = DFANlastref();
    ASSERT_NE(annref, 0);

    const int32_t fid = Hopen(path.c_str(), DFACC_RDWR, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hdeldd(fid, DFTAG_DIL, annref), FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    DFANclear();
    char buf[64] = {};
    EXPECT_EQ(DFANgetlabel(path.c_str(), 800, 1, buf, sizeof(buf)), FAIL);
    EXPECT_EQ(DFANgetlablen(path.c_str(), 800, 1), FAIL);
  }

  // 3. Put label with corrupted 2-byte data (less than 4-byte header)
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    constexpr uint16_t annref = 88;
    const uint8_t short_header[2] = {0, 1};
    ASSERT_NE(
        Hputelement(fid, DFTAG_DIL, annref, short_header, sizeof(short_header)),
        FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    DFANclear();
    // DFANIlocate will attempt to read 4 bytes for datadi and fail
    const int32_t rfid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(rfid, FAIL);
    EXPECT_EQ(DFANIlocate(rfid, DFAN_LABEL, 800, 1), 0);
    ASSERT_NE(Hclose(rfid), FAIL);
  }

  // 4. DFANIlocate with tag == 0 returns 1 (when annotations exist)
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    EXPECT_EQ(DFANputlabel(path.c_str(), 900, 1, const_cast<char*>("Test")),
              SUCCEED);
    const int32_t rfid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(rfid, FAIL);
    EXPECT_EQ(DFANIlocate(rfid, DFAN_LABEL, 0, 0), 1);
    ASSERT_NE(Hclose(rfid), FAIL);
  }

  // 5. Truncate file at annotation offset so Hread in DFANIgetann fails
  {
    EXPECT_EQ(DFANputlabel(path.c_str(), 950, 1,
                           const_cast<char*>("Longer Label String")),
              SUCCEED);
    const uint16_t annref = DFANlastref();
    int32_t off = 0;
    {
      const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
      ASSERT_NE(fid, FAIL);
      off = Hoffset(fid, DFTAG_DIL, annref);
      ASSERT_GT(off, 0);
      ASSERT_NE(Hclose(fid), FAIL);

      FILE* f = fopen(path.c_str(), "r+b");
      ASSERT_NE(f, nullptr);
      int fd = fileno(f);
      ASSERT_EQ(ftruncate(fd, off), 0);
      fclose(f);
    }
    char buf[64] = {};
    EXPECT_EQ(DFANgetlabel(path.c_str(), 950, 1, buf, sizeof(buf)), FAIL);
  }

  // 5b. Read-only fid for DFANaddfid and DFANaddfds (Hputelement fails)
  {
    const std::string ro_path = dir + "/dfan_ro.hdf";
    (void)remove(ro_path.c_str());
    const absl::Cleanup ro_cleanup = [&ro_path] {
      (void)remove(ro_path.c_str());
    };

    const int32_t cr_fid = Hopen(ro_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(cr_fid, FAIL);
    ASSERT_NE(Hclose(cr_fid), FAIL);

    const int32_t ro_fid = Hopen(ro_path.c_str(), DFACC_READ, 0);
    ASSERT_NE(ro_fid, FAIL);
    EXPECT_EQ(DFANaddfid(ro_fid, const_cast<char*>("read-only")), FAIL);
    EXPECT_EQ(DFANaddfds(ro_fid, const_cast<char*>("read-only"), 9), FAIL);
    ASSERT_NE(Hclose(ro_fid), FAIL);
  }

  // 6. HDreuse_tagref failure in DFANIputann
  {
    const std::string reuse_path = dir + "/dfan_reuse_fail.hdf";
    (void)remove(reuse_path.c_str());
    const absl::Cleanup reuse_cleanup = [&reuse_path] {
      (void)remove(reuse_path.c_str());
    };

    const int32_t fid = Hopen(reuse_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    DFANclear();
    // Cache reuse_path as Lastfile so DFANIopen does not clear DFANdir
    (void)DFANgetlablen(reuse_path.c_str(), 1, 1);
    // Insert directory entry pointing to non-existent DD 9999
    ASSERT_EQ(DFANIaddentry(DFAN_LABEL, 9999, 123, 456), SUCCEED);
    // Put label tries to reuse non-existent DD 9999 -> fails
    EXPECT_EQ(DFANputlabel(reuse_path.c_str(), 123, 456,
                           const_cast<char*>("will fail")),
              FAIL);
  }

  // 7. Missing DD in DFANIgetannlen
  {
    const std::string bad_dd_path = dir + "/dfan_bad_dd.hdf";
    (void)remove(bad_dd_path.c_str());
    const absl::Cleanup bad_cleanup = [&bad_dd_path] {
      (void)remove(bad_dd_path.c_str());
    };

    const int32_t fid = Hopen(bad_dd_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    DFANclear();
    (void)DFANgetlablen(bad_dd_path.c_str(), 1, 1);
    ASSERT_EQ(DFANIaddentry(DFAN_LABEL, 9998, 124, 456), SUCCEED);
    EXPECT_EQ(DFANgetlablen(bad_dd_path.c_str(), 124, 456), FAIL);
  }

  // 8. Missing DD in DFANIgetann
  {
    const std::string bad_get_path = dir + "/dfan_bad_get.hdf";
    (void)remove(bad_get_path.c_str());
    const absl::Cleanup bad_cleanup = [&bad_get_path] {
      (void)remove(bad_get_path.c_str());
    };

    const int32_t fid = Hopen(bad_get_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    DFANclear();
    (void)DFANgetlablen(bad_get_path.c_str(), 1, 1);
    ASSERT_EQ(DFANIaddentry(DFAN_LABEL, 9997, 125, 456), SUCCEED);
    char buf[64] = {};
    EXPECT_EQ(DFANgetlabel(bad_get_path.c_str(), 125, 456, buf, sizeof(buf)),
              FAIL);
  }

  // 9. Missing DD in DFANIlablist
  {
    const std::string bad_list_path = dir + "/dfan_bad_list.hdf";
    (void)remove(bad_list_path.c_str());
    const absl::Cleanup bad_cleanup = [&bad_list_path] {
      (void)remove(bad_list_path.c_str());
    };

    const int32_t fid = Hopen(bad_list_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    const uint8_t dummy = 1;
    ASSERT_NE(Hputelement(fid, 350, 1, &dummy, 1), FAIL);
    ASSERT_NE(Hputelement(fid, 350, 2, &dummy, 1), FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    // Put two labels then delete DD for first one
    EXPECT_EQ(
        DFANputlabel(bad_list_path.c_str(), 350, 1, const_cast<char*>("temp1")),
        SUCCEED);
    const uint16_t annref1 = DFANlastref();
    ASSERT_NE(annref1, 0);

    EXPECT_EQ(
        DFANputlabel(bad_list_path.c_str(), 350, 2, const_cast<char*>("temp2")),
        SUCCEED);

    const int32_t edit_fid = Hopen(bad_list_path.c_str(), DFACC_RDWR, 0);
    ASSERT_NE(edit_fid, FAIL);
    ASSERT_NE(Hdeldd(edit_fid, DFTAG_DIL, annref1), FAIL);
    ASSERT_NE(Hclose(edit_fid), FAIL);

    uint16_t reflist[5] = {};
    char labellist[160] = {};
    EXPECT_EQ(
        DFANlablist(bad_list_path.c_str(), 350, reflist, labellist, 5, 32, 1),
        FAIL);
  }

  // 10. Reopen same file fails when file was removed
  {
    const std::string reopen_path = dir + "/dfan_reopen_fail.hdf";
    (void)remove(reopen_path.c_str());
    const absl::Cleanup reopen_cleanup = [&reopen_path] {
      (void)remove(reopen_path.c_str());
    };

    const int32_t fid = Hopen(reopen_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    DFANclear();
    // 1st open sets Lastfile
    (void)DFANgetlablen(reopen_path.c_str(), 1, 1);
    // Remove the file while Lastfile still points to it
    (void)remove(reopen_path.c_str());
    // Next call with same filename goes to reopen branch (line 694-695) and
    // fails
    EXPECT_EQ(DFANgetlablen(reopen_path.c_str(), 1, 1), FAIL);
  }

  // 11. Add multiple labels to populate and reuse empty slot in DFANdir
  {
    const std::string multi_label_path = dir + "/dfan_multi_label.hdf";
    (void)remove(multi_label_path.c_str());
    const absl::Cleanup multi_cleanup = [&multi_label_path] {
      (void)remove(multi_label_path.c_str());
    };

    const int32_t fid = Hopen(multi_label_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    DFANclear();
    // First label creates DFANdir with 16 slots, using slot 0
    EXPECT_EQ(DFANputlabel(multi_label_path.c_str(), 700, 1,
                           const_cast<char*>("Label 1")),
              SUCCEED);
    // Second label on different ref uses slot 1 (line 859)
    EXPECT_EQ(DFANputlabel(multi_label_path.c_str(), 700, 2,
                           const_cast<char*>("Label 2")),
              SUCCEED);
  }

  // 12. Truncated label data in DFANIgetann (lines 1038 and 1040-1042)
  {
    const std::string trunc_ann_path = dir + "/dfan_trunc_ann.hdf";
    (void)remove(trunc_ann_path.c_str());
    const absl::Cleanup trunc_cleanup = [&trunc_ann_path] {
      (void)remove(trunc_ann_path.c_str());
    };

    const int32_t fid = Hopen(trunc_ann_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    DFANclear();
    EXPECT_EQ(DFANputlabel(trunc_ann_path.c_str(), 800, 1,
                           const_cast<char*>("A Long Label For Testing")),
              SUCCEED);
    const uint16_t annref = DFANlastref();

    // Find offset of the label data element
    int32_t read_fid = Hopen(trunc_ann_path.c_str(), DFACC_READ, 0);
    ASSERT_NE(read_fid, FAIL);
    int32_t off = Hoffset(read_fid, DFTAG_DIL, annref);
    ASSERT_NE(off, FAIL);
    ASSERT_NE(Hclose(read_fid), FAIL);

    // Truncate at off + 4 (so 4-byte datadi header succeeds, but annlen read
    // fails -> line 1040-1042)
    {
      FILE* f = fopen(trunc_ann_path.c_str(), "r+b");
      ASSERT_NE(f, nullptr);
      int fd = fileno(f);
      ASSERT_EQ(ftruncate(fd, off + 4), 0);
      fclose(f);
    }
    char buf[64] = {};
    EXPECT_EQ(DFANgetlabel(trunc_ann_path.c_str(), 800, 1, buf, sizeof(buf)),
              FAIL);

    // Truncate at off (so 4-byte datadi read fails -> line 1038)
    {
      FILE* f = fopen(trunc_ann_path.c_str(), "r+b");
      ASSERT_NE(f, nullptr);
      int fd = fileno(f);
      ASSERT_EQ(ftruncate(fd, off), 0);
      fclose(f);
    }
    EXPECT_EQ(DFANgetlabel(trunc_ann_path.c_str(), 800, 1, buf, sizeof(buf)),
              FAIL);
  }

  // 13. Lablist with truncated annotation elements (lines 788, 1257-1259,
  // 1274-1276, 1287-1289)
  {
    // A: Truncate at off so DFANIlocate Hread(aid, 4, datadi) fails (lines 788,
    // 1257-1259)
    const std::string trunc_lablist_a = dir + "/dfan_trunc_lablist_a.hdf";
    (void)remove(trunc_lablist_a.c_str());
    const absl::Cleanup a_cleanup = [&trunc_lablist_a] {
      (void)remove(trunc_lablist_a.c_str());
    };

    const int32_t fid_a = Hopen(trunc_lablist_a.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid_a, FAIL);
    const uint8_t dummy = 1;
    ASSERT_NE(Hputelement(fid_a, 900, 10, &dummy, 1), FAIL);
    ASSERT_NE(Hclose(fid_a), FAIL);

    DFANclear();
    EXPECT_EQ(DFANputlabel(trunc_lablist_a.c_str(), 900, 10,
                           const_cast<char*>("Label A Long Enough")),
              SUCCEED);
    const uint16_t annref_a = DFANlastref();

    int32_t rfid_a = Hopen(trunc_lablist_a.c_str(), DFACC_READ, 0);
    ASSERT_NE(rfid_a, FAIL);
    int32_t off_a = Hoffset(rfid_a, DFTAG_DIL, annref_a);
    ASSERT_NE(off_a, FAIL);
    ASSERT_NE(Hclose(rfid_a), FAIL);

    // Truncate at off_a
    {
      FILE* f = fopen(trunc_lablist_a.c_str(), "r+b");
      ASSERT_NE(f, nullptr);
      int fd = fileno(f);
      ASSERT_EQ(ftruncate(fd, off_a), 0);
      fclose(f);
    }
    // Clear directory so DFANlablist calls DFANIlocate -> DFANIlocate fails ->
    // lines 788, 1257-1259
    DFANclear();
    uint16_t reflist[5] = {};
    char labellist[160] = {};
    EXPECT_EQ(
        DFANlablist(trunc_lablist_a.c_str(), 900, reflist, labellist, 5, 32, 1),
        FAIL);

    // B: DFANdir already populated; truncate at off_b so DFANIlablist line
    // 1274-1276 fails
    const std::string trunc_lablist_b = dir + "/dfan_trunc_lablist_b.hdf";
    (void)remove(trunc_lablist_b.c_str());
    const absl::Cleanup b_cleanup = [&trunc_lablist_b] {
      (void)remove(trunc_lablist_b.c_str());
    };

    const int32_t fid_b = Hopen(trunc_lablist_b.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid_b, FAIL);
    ASSERT_NE(Hputelement(fid_b, 900, 10, &dummy, 1), FAIL);
    ASSERT_NE(Hclose(fid_b), FAIL);

    DFANclear();
    EXPECT_EQ(DFANputlabel(trunc_lablist_b.c_str(), 900, 10,
                           const_cast<char*>("Label B Long Enough")),
              SUCCEED);
    const uint16_t annref_b = DFANlastref();

    int32_t rfid_b = Hopen(trunc_lablist_b.c_str(), DFACC_READ, 0);
    ASSERT_NE(rfid_b, FAIL);
    int32_t off_b = Hoffset(rfid_b, DFTAG_DIL, annref_b);
    ASSERT_NE(off_b, FAIL);
    ASSERT_NE(Hclose(rfid_b), FAIL);

    // Truncate at off_b without calling DFANclear() (DFANdir remains populated)
    {
      FILE* f = fopen(trunc_lablist_b.c_str(), "r+b");
      ASSERT_NE(f, nullptr);
      int fd = fileno(f);
      ASSERT_EQ(ftruncate(fd, off_b), 0);
      fclose(f);
    }
    EXPECT_EQ(
        DFANlablist(trunc_lablist_b.c_str(), 900, reflist, labellist, 5, 32, 1),
        FAIL);

    // C: Truncate at off_c + 4 so 4-byte header succeeds but label read fails
    // (lines 1287-1289)
    const std::string trunc_lablist_c = dir + "/dfan_trunc_lablist_c.hdf";
    (void)remove(trunc_lablist_c.c_str());
    const absl::Cleanup c_cleanup = [&trunc_lablist_c] {
      (void)remove(trunc_lablist_c.c_str());
    };

    const int32_t fid_c = Hopen(trunc_lablist_c.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid_c, FAIL);
    ASSERT_NE(Hputelement(fid_c, 900, 10, &dummy, 1), FAIL);
    ASSERT_NE(Hclose(fid_c), FAIL);

    DFANclear();
    EXPECT_EQ(DFANputlabel(trunc_lablist_c.c_str(), 900, 10,
                           const_cast<char*>("Label C Long Enough")),
              SUCCEED);
    const uint16_t annref_c = DFANlastref();

    int32_t rfid_c = Hopen(trunc_lablist_c.c_str(), DFACC_READ, 0);
    ASSERT_NE(rfid_c, FAIL);
    int32_t off_c = Hoffset(rfid_c, DFTAG_DIL, annref_c);
    ASSERT_NE(off_c, FAIL);
    ASSERT_NE(Hclose(rfid_c), FAIL);

    // Truncate at off_c + 4 without calling DFANclear()
    {
      FILE* f = fopen(trunc_lablist_c.c_str(), "r+b");
      ASSERT_NE(f, nullptr);
      int fd = fileno(f);
      ASSERT_EQ(ftruncate(fd, off_c + 4), 0);
      fclose(f);
    }
    EXPECT_EQ(
        DFANlablist(trunc_lablist_c.c_str(), 900, reflist, labellist, 5, 32, 1),
        FAIL);
  }

  // 14. Truncated file annotation in DFANIgetfann (lines 1493-1495)
  {
    const std::string trunc_fid_path = dir + "/dfan_trunc_fid.hdf";
    (void)remove(trunc_fid_path.c_str());
    const absl::Cleanup fid_cleanup = [&trunc_fid_path] {
      (void)remove(trunc_fid_path.c_str());
    };

    const int32_t fid = Hopen(trunc_fid_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    EXPECT_EQ(DFANaddfid(fid, const_cast<char*>("File Annotation Test")),
              SUCCEED);
    const uint16_t annref = DFANlastref();

    int32_t off = Hoffset(fid, DFTAG_FID, annref);
    ASSERT_NE(off, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    // Truncate at off
    {
      FILE* f = fopen(trunc_fid_path.c_str(), "r+b");
      ASSERT_NE(f, nullptr);
      int fd = fileno(f);
      ASSERT_EQ(ftruncate(fd, off), 0);
      fclose(f);
    }

    int32_t open_fid = Hopen(trunc_fid_path.c_str(), DFACC_READ, 0);
    ASSERT_NE(open_fid, FAIL);
    char buf[64] = {};
    EXPECT_EQ(DFANgetfid(open_fid, buf, sizeof(buf), 1), FAIL);
    ASSERT_NE(Hclose(open_fid), FAIL);
  }
}

}  // namespace
}  // namespace hdf4
