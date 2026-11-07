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

// Tests for V3.X stubs for V4.0 H-level I/O routines
// (third_party/hdf4/hdf/src/dfstubs.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/dfstubs.c

#include <cstdint>
#include <cstdlib>
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

#include "third_party/hdf4/hdf/src/df.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"

namespace hdf4 {
namespace {

using ::testing::ElementsAre;

std::string GetTempFilePath(const std::string& name) {
  return (std::filesystem::path(::testing::TempDir()) / name).string();
}

class DfstubsTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }

  void TearDown() override { HEclear(); }
};

TEST_F(DfstubsTest, OpenAndCloseErrorsAndValid) {
  std::string path = GetTempFilePath("dfstubs_open_close.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  // 1. Open non-existent file for read fails.
  std::string non_existent = GetTempFilePath("dfstubs_no_such_file.hdf");
  EXPECT_EQ(nullptr, DFopen(non_existent.data(), DFACC_READ, 0));
  EXPECT_EQ(DFE_BADOPEN, DFerrno());

  // 2. Open for create succeeds.
  DF* dfile = DFopen(path.data(), DFACC_CREATE, 0);
  ASSERT_NE(nullptr, dfile);
  EXPECT_EQ(DFE_NONE, DFerrno());

  // 3. Opening another file while already open fails with DFE_TOOMANY.
  std::string second_path = GetTempFilePath("dfstubs_second.hdf");
  EXPECT_EQ(nullptr, DFopen(second_path.data(), DFACC_CREATE, 0));
  EXPECT_EQ(DFE_TOOMANY, DFerrno());

  // 4. Close with invalid pointer fails.
  EXPECT_EQ(FAIL, DFclose(nullptr));
  EXPECT_EQ(DFE_NOTOPEN, DFerrno());

  // 5. Close valid file succeeds.
  EXPECT_EQ(0, DFclose(dfile));
  EXPECT_EQ(DFE_NONE, DFerrno());

  // 6. Close again fails since file is no longer open.
  EXPECT_EQ(FAIL, DFclose(dfile));
  EXPECT_EQ(DFE_NOTOPEN, DFerrno());
}

TEST_F(DfstubsTest, PutAndGetElement) {
  std::string path = GetTempFilePath("dfstubs_put_get.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  DF* dfile = DFopen(path.data(), DFACC_CREATE, 0);
  ASSERT_NE(nullptr, dfile);
  absl::Cleanup close_file = [dfile] { DFclose(dfile); };

  // Put element on invalid file pointer.
  char data[] = "Hello HDF4";
  EXPECT_EQ(-1, DFputelement(nullptr, 100, 1, data, sizeof(data)));
  EXPECT_EQ(DFE_NOTOPEN, DFerrno());

  // Put element on valid file.
  EXPECT_EQ(sizeof(data), DFputelement(dfile, 100, 1, data, sizeof(data)));
  EXPECT_EQ(DFE_NONE, DFerrno());

  // Get element on invalid file pointer.
  char read_buf[32] = {};
  EXPECT_EQ(-1, DFgetelement(nullptr, 100, 1, read_buf));
  EXPECT_EQ(DFE_NOTOPEN, DFerrno());

  // Get element on non-existent tag/ref.
  EXPECT_EQ(-1, DFgetelement(dfile, 999, 999, read_buf));
  EXPECT_NE(DFE_NONE, DFerrno());

  // Get valid element.
  std::memset(read_buf, 0, sizeof(read_buf));
  EXPECT_EQ(sizeof(data), DFgetelement(dfile, 100, 1, read_buf));
  EXPECT_STREQ(data, read_buf);
}

TEST_F(DfstubsTest, DescriptorsAndNumber) {
  std::string path = GetTempFilePath("dfstubs_desc_num.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  DF* dfile = DFopen(path.data(), DFACC_CREATE, 0);
  ASSERT_NE(nullptr, dfile);
  absl::Cleanup close_file = [dfile] { DFclose(dfile); };

  char elem1[] = "Element One";
  char elem2[] = "Element Two";
  ASSERT_GT(DFputelement(dfile, 200, 1, elem1, sizeof(elem1)), 0);
  ASSERT_GT(DFputelement(dfile, 200, 2, elem2, sizeof(elem2)), 0);

  // Number checks.
  EXPECT_EQ(-1, DFnumber(nullptr, 200));
  EXPECT_EQ(DFE_NOTOPEN, DFerrno());

  EXPECT_EQ(2, DFnumber(dfile, 200));
  EXPECT_EQ(0, DFnumber(dfile, 999));
  EXPECT_GE(DFnumber(dfile, DFTAG_WILDCARD), 2);

  // Descriptors checks.
  DFdesc desc_list[10] = {};
  EXPECT_EQ(-1, DFdescriptors(nullptr, desc_list, 1, 10));
  EXPECT_EQ(DFE_NOTOPEN, DFerrno());

  // Begin out of range.
  EXPECT_EQ(-1, DFdescriptors(dfile, desc_list, 1000, 5));

  // Valid descriptors query starting at begin = 1.
  int count = DFdescriptors(dfile, desc_list, 1, 10);
  EXPECT_GE(count, 2);

  // Starting at begin = 2.
  count = DFdescriptors(dfile, desc_list, 2, 10);
  EXPECT_GE(count, 1);

  // Exact number query hits loop completion without break.
  count = DFdescriptors(dfile, desc_list, 1, 1);
  EXPECT_EQ(1, count);
}

TEST_F(DfstubsTest, FindOperations) {
  std::string path = GetTempFilePath("dfstubs_find.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  DF* dfile = DFopen(path.data(), DFACC_CREATE, 0);
  ASSERT_NE(nullptr, dfile);
  absl::Cleanup close_file = [dfile] { DFclose(dfile); };

  char data[] = "Searchable";
  ASSERT_GT(DFputelement(dfile, 300, 10, data, sizeof(data)), 0);
  ASSERT_GT(DFputelement(dfile, 300, 20, data, sizeof(data)), 0);

  // DFsetfind checks.
  EXPECT_EQ(-1, DFsetfind(nullptr, 300, 0));
  EXPECT_EQ(DFE_NOTOPEN, DFerrno());

  EXPECT_EQ(0, DFsetfind(dfile, 300, 0));

  // DFfind checks.
  DFdesc found_desc = {};
  EXPECT_EQ(-1, DFfind(nullptr, &found_desc));
  EXPECT_EQ(DFE_NOTOPEN, DFerrno());

  // First find (DFSRCH_NEW).
  EXPECT_EQ(0, DFfind(dfile, &found_desc));
  EXPECT_EQ(300, found_desc.tag);
  EXPECT_EQ(10, found_desc.ref);

  // Second find (DFSRCH_OLD).
  EXPECT_EQ(0, DFfind(dfile, &found_desc));
  EXPECT_EQ(300, found_desc.tag);
  EXPECT_EQ(20, found_desc.ref);

  // Third find (no more elements).
  EXPECT_EQ(-1, DFfind(dfile, &found_desc));
  EXPECT_EQ(DFE_NOMATCH, DFerrno());
  EXPECT_EQ(0, found_desc.tag);
  EXPECT_EQ(0, found_desc.ref);
}

TEST_F(DfstubsTest, AccessReadWriteSeek) {
  std::string path = GetTempFilePath("dfstubs_rw_seek.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  // 1. Create file and write an element via DFaccess/DFwrite.
  {
    DF* dfile = DFopen(path.data(), DFACC_CREATE, 0);
    ASSERT_NE(nullptr, dfile);
    absl::Cleanup close_file = [dfile] { DFclose(dfile); };

    // Bad access mode.
    EXPECT_EQ(-1, DFaccess(dfile, 400, 1, const_cast<char*>("x")));
    EXPECT_EQ(DFE_BADACC, DFerrno());

    EXPECT_EQ(-1, DFaccess(nullptr, 400, 1, const_cast<char*>("w")));
    EXPECT_EQ(DFE_NOTOPEN, DFerrno());

    // DFstart is an alias for DFaccess.
    EXPECT_EQ(0, DFstart(dfile, 400, 1, const_cast<char*>("w")));

    // Write on absent element.
    char chunk1[] = "Hello ";
    EXPECT_EQ(6, DFwrite(dfile, chunk1, 6));

    // Seek within element.
    EXPECT_EQ(-1, DFseek(nullptr, 0));
    EXPECT_EQ(DFE_NOTOPEN, DFerrno());

    EXPECT_EQ(-1, DFseek(dfile, 1000));  // Past end.
    EXPECT_EQ(DFE_BADSEEK, DFerrno());

    EXPECT_EQ(0, DFseek(dfile, 6));

    // Write on resident element where size > DFelsize fails with DFE_NOTENOUGH.
    char chunk2[] = "World!";
    EXPECT_EQ(-1, DFwrite(dfile, chunk2, 6));
    EXPECT_EQ(DFE_NOTENOUGH, DFerrno());

    // Write on resident element where size <= DFelsize.
    EXPECT_EQ(0, DFseek(dfile, 0));
    EXPECT_EQ(6, DFwrite(dfile, const_cast<char*>("HELLO "), 6));

    // Append on non-existent element fails.
    EXPECT_EQ(-1, DFaccess(dfile, 400, 999, const_cast<char*>("a")));
    EXPECT_NE(DFE_NONE, DFerrno());

    // Write on existing element sets resident state.
    EXPECT_EQ(0, DFaccess(dfile, 400, 1, const_cast<char*>("w")));

    // Append mode.
    EXPECT_EQ(0, DFaccess(dfile, 400, 1, const_cast<char*>("a")));
    EXPECT_EQ(6, DFwrite(dfile, const_cast<char*>("World!"), 6));
  }

  // 2. Open read-only and read data back.
  {
    DF* dfile = DFopen(path.data(), DFACC_READ, 0);
    ASSERT_NE(nullptr, dfile);
    absl::Cleanup close_file = [dfile] { DFclose(dfile); };

    // Try 'w' access on read-only file fails.
    EXPECT_EQ(-1, DFaccess(dfile, 400, 1, const_cast<char*>("w")));
    EXPECT_EQ(DFE_BADACC, DFerrno());

    // Try 'a' access on read-only file fails.
    EXPECT_EQ(-1, DFaccess(dfile, 400, 1, const_cast<char*>("a")));
    EXPECT_EQ(DFE_BADACC, DFerrno());

    // Try 'r' on non-existent element fails.
    EXPECT_EQ(-1, DFaccess(dfile, 999, 999, const_cast<char*>("r")));

    // Valid 'r' access.
    EXPECT_EQ(0, DFaccess(dfile, 400, 1, const_cast<char*>("r")));

    // Read error on null dfile.
    char buf[32] = {};
    EXPECT_EQ(-1, DFread(nullptr, buf, 12));
    EXPECT_EQ(DFE_NOTOPEN, DFerrno());

    // Read valid portion.
    EXPECT_EQ(12, DFread(dfile, buf, 12));
    EXPECT_STREQ("HELLO World!", buf);

    // Read at EOF returns 0.
    EXPECT_EQ(0, DFread(dfile, buf, 5));

    // DFwrite fails when element access mode is read.
    EXPECT_EQ(-1, DFwrite(dfile, buf, 5));
    EXPECT_EQ(DFE_BADACC, DFerrno());
  }
}

TEST_F(DfstubsTest, DupDelNewrefIshdfUpdateStat) {
  std::string path = GetTempFilePath("dfstubs_misc.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  // DFishdf before creation.
  EXPECT_EQ(-1, DFishdf(const_cast<char*>(path.c_str())));

  DF* dfile = DFopen(path.data(), DFACC_CREATE, 0);
  ASSERT_NE(nullptr, dfile);
  absl::Cleanup close_file = [dfile] { DFclose(dfile); };

  // DFishdf on open HDF file.
  EXPECT_EQ(0, DFishdf(const_cast<char*>(path.c_str())));

  // DFupdate and DFstat.
  EXPECT_EQ(-1, DFupdate(nullptr));
  EXPECT_EQ(DFE_NOTOPEN, DFerrno());
  EXPECT_EQ(0, DFupdate(dfile));

  DFdata dfinfo = {};
  EXPECT_EQ(-1, DFstat(nullptr, &dfinfo));
  EXPECT_EQ(DFE_NOTOPEN, DFerrno());
  EXPECT_EQ(0, DFstat(dfile, &dfinfo));

  // DFnewref.
  EXPECT_EQ(0, DFnewref(nullptr));
  EXPECT_EQ(DFE_NOTOPEN, DFerrno());
  const uint16 ref = DFnewref(dfile);
  EXPECT_GT(ref, 0);

  // Write element, dup, and del.
  char data[] = "DupDel";
  ASSERT_GT(DFputelement(dfile, 500, ref, data, sizeof(data)), 0);

  EXPECT_EQ(-1, DFdup(nullptr, 501, 1, 500, ref));
  EXPECT_EQ(DFE_NOTOPEN, DFerrno());
  EXPECT_EQ(0, DFdup(dfile, 501, 1, 500, ref));

  // Verify duplicated element exists.
  char read_buf[16] = {};
  EXPECT_EQ(sizeof(data), DFgetelement(dfile, 501, 1, read_buf));
  EXPECT_STREQ(data, read_buf);

  // Delete duplicated DD.
  EXPECT_EQ(-1, DFdel(nullptr, 501, 1));
  EXPECT_EQ(DFE_NOTOPEN, DFerrno());
  EXPECT_EQ(0, DFdel(dfile, 501, 1));
  EXPECT_EQ(-1, DFgetelement(dfile, 501, 1, read_buf));
}

TEST_F(DfstubsTest, DFIHelperRoutines) {
  std::string path = GetTempFilePath("dfstubs_helper.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  DF* dfile = DFopen(path.data(), DFACC_CREATE, 0);
  ASSERT_NE(nullptr, dfile);

  // DFIerr with valid file: closes file and preserves DFerror.
  DFerror = DFE_BADACC;
  EXPECT_EQ(-1, DFIerr(dfile));
  EXPECT_EQ(DFE_BADACC, DFerrno());

  // DFIerr with nullptr.
  DFerror = DFE_NOMATCH;
  EXPECT_EQ(-1, DFIerr(nullptr));
  EXPECT_EQ(DFE_NOMATCH, DFerrno());

  // DFIgetspace and DFIfreespace.
  void* ptr = DFIgetspace(64);
  ASSERT_NE(nullptr, ptr);
  EXPECT_EQ(nullptr, DFIfreespace(ptr));

  // DFIc2fstr.
  char c_str[] = "Fortran";
  EXPECT_EQ(0, DFIc2fstr(c_str, sizeof(c_str)));

  // DFIf2cstring.
  char f_str[] = "Converted   ";
  char* result = DFIf2cstring(f_str, 12);
  ASSERT_NE(nullptr, result);
  EXPECT_STREQ("Converted", result);
  std::free(result);
}

}  // namespace
}  // namespace hdf4
