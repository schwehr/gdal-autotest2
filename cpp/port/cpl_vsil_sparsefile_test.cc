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
//
// Tests for GDAL's /vsisparse/ virtual file system.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil_sparsefile.cpp

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

TEST(CplVsilSparsefileTest, HandlerErrorsAndUnsupportedOperations) {
  auto* handler = VSIFileManager::GetHandler("/vsisparse/");
  ASSERT_NE(nullptr, handler);

  // errno is checked right after calling the handler methods directly, since
  // the VSIF*L wrappers in cpl_vsil.cpp may clobber errno on the way back.

  // Direct Open with non-/vsisparse/ prefix returns nullptr.
  EXPECT_EQ(nullptr,
            handler->Open("/not_sparse/foo.xml", "rb", false, nullptr));

  // Opening for write fails with EACCES.
  errno = 0;
  EXPECT_EQ(nullptr,
            handler->Open("/vsisparse//vsimem/foo.xml", "wb", false, nullptr));
  EXPECT_EQ(EACCES, errno);
  EXPECT_EQ(nullptr, VSIFOpenL("/vsisparse//vsimem/foo.xml", "wb"));

  // Non-existent XML file returns nullptr / -1.
  EXPECT_EQ(nullptr, VSIFOpenL("/vsisparse//vsimem/does_not_exist.xml", "rb"));
  VSIStatBufL stat_buf;
  EXPECT_EQ(-1, VSIStatL("/vsisparse//vsimem/does_not_exist.xml", &stat_buf));

  // Malformed XML file fails CPLParseXMLFile.
  const std::string bad_xml = "/vsimem/bad_sparse.xml";
  VsiMemTempWrapper bad_xml_wrapper(bad_xml, "<VSISparseFile>");
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr,
              VSIFOpenL(absl::StrCat("/vsisparse/", bad_xml).c_str(), "rb"));
  }

  // Unsupported filesystem methods return -1 or nullptr with EACCES.
  errno = 0;
  EXPECT_EQ(-1, handler->Unlink("/vsisparse//vsimem/foo.xml"));
  EXPECT_EQ(EACCES, errno);
  errno = 0;
  EXPECT_EQ(-1, handler->Mkdir("/vsisparse//vsimem/dir", 0755));
  EXPECT_EQ(EACCES, errno);
  errno = 0;
  EXPECT_EQ(-1, handler->Rmdir("/vsisparse//vsimem/dir"));
  EXPECT_EQ(EACCES, errno);
  errno = 0;
  EXPECT_EQ(nullptr, handler->ReadDirEx("/vsisparse//vsimem/dir", 0));
  EXPECT_EQ(EACCES, errno);

  // The same operations through the public VSI API also fail.
  EXPECT_EQ(-1, VSIUnlink("/vsisparse//vsimem/foo.xml"));
  EXPECT_EQ(-1, VSIMkdir("/vsisparse//vsimem/dir", 0755));
  EXPECT_EQ(-1, VSIRmdir("/vsisparse//vsimem/dir"));
  EXPECT_EQ(nullptr, VSIReadDirEx("/vsisparse//vsimem/dir", 0));
}

TEST(CplVsilSparsefileTest, ReadSeekTellAndStatAcrossRegions) {
  const std::string subfile1 = "/vsimem/sparse_part1.bin";
  const std::string subfile2 = "/vsimem/sparse_part2.bin";
  autotest2::VsiMemTempWrapper wrap1(subfile1, "0123456789");
  autotest2::VsiMemTempWrapper wrap2(subfile2, "ABCDEF");

  // XML without top-level <Length> so overall length is computed from regions:
  // - [0..4): SubfileRegion from sparse_part1.bin (relative="1") at source
  //   offset 2 ("2345")
  // - [4..6): unmapped hole (implicitly zero-filled)
  // - [6..10): ConstantRegion with Value=65 ('A')
  // - [10..14): SubfileRegion from /vsimem/sparse_part2.bin at src 1 ("BCDE")
  // - Ignored comment and <IgnoredTag/>
  const std::string xml_content = R"xml(<VSISparseFile>
  <!-- comment node -->
  <IgnoredTag/>
  <SubfileRegion>
    <Filename relative="1">sparse_part1.bin</Filename>
    <DestinationOffset>0</DestinationOffset>
    <SourceOffset>2</SourceOffset>
    <RegionLength>4</RegionLength>
  </SubfileRegion>
  <ConstantRegion>
    <DestinationOffset>6</DestinationOffset>
    <RegionLength>4</RegionLength>
    <Value>65</Value>
  </ConstantRegion>
  <SubfileRegion>
    <Filename relative="0">/vsimem/sparse_part2.bin</Filename>
    <DestinationOffset>10</DestinationOffset>
    <SourceOffset>1</SourceOffset>
    <RegionLength>4</RegionLength>
  </SubfileRegion>
</VSISparseFile>)xml";

  const std::string xml_file = "/vsimem/sparse_test.xml";
  autotest2::VsiMemTempWrapper xml_wrap(xml_file, xml_content);
  const std::string sparse_path = absl::StrCat("/vsisparse/", xml_file);

  VSIStatBufL stat_buf;
  ASSERT_EQ(0, VSIStatL(sparse_path.c_str(), &stat_buf));
  EXPECT_EQ(14, stat_buf.st_size);

  VSILFILE* fp = VSIFOpenL(sparse_path.c_str(), "rb");
  ASSERT_NE(nullptr, fp);

  EXPECT_EQ(0, VSIFTellL(fp));
  EXPECT_EQ(0, VSIFEofL(fp));
  EXPECT_EQ(0, VSIFErrorL(fp));

  // Write is unsupported and sets EBADF (checked via the handle directly so
  // that the VSIFWriteL wrapper cannot clobber errno).
  errno = 0;
  EXPECT_EQ(0, fp->Write("x", 1, 1));
  EXPECT_EQ(EBADF, errno);
  EXPECT_EQ(0, VSIFWriteL("x", 1, 1, fp));

  // Zero-byte read returns 0.
  char buf[32] = {};
  EXPECT_EQ(0, VSIFReadL(buf, 0, 5, fp));

  // Read [0..4) from first SubfileRegion ("2345").
  EXPECT_EQ(4, VSIFReadL(buf, 1, 4, fp));
  EXPECT_EQ(4, VSIFTellL(fp));
  EXPECT_EQ(0, memcmp(buf, "2345", 4));

  // Read [4..6) from unmapped hole (zero-filled).
  memset(buf, 'X', sizeof(buf));
  EXPECT_EQ(2, VSIFReadL(buf, 1, 2, fp));
  EXPECT_EQ(6, VSIFTellL(fp));
  EXPECT_EQ('\0', buf[0]);
  EXPECT_EQ('\0', buf[1]);

  // Read across ConstantRegion [6..10) and SubfileRegion [10..14) and past EOF
  // in a single call (requests 20, gets 8).
  EXPECT_EQ(8, VSIFReadL(buf, 1, 20, fp));
  EXPECT_EQ(14, VSIFTellL(fp));
  EXPECT_NE(0, VSIFEofL(fp));
  EXPECT_EQ(0, memcmp(buf, "AAAABCDE", 8));

  // Reading at or past overall length returns 0 and sets EOF.
  EXPECT_EQ(0, VSIFReadL(buf, 1, 1, fp));
  EXPECT_NE(0, VSIFEofL(fp));
  VSIFClearErrL(fp);
  EXPECT_EQ(0, VSIFEofL(fp));
  EXPECT_EQ(0, VSIFErrorL(fp));

  // Seek tests: SEEK_SET, SEEK_CUR, SEEK_END, and invalid whence.
  EXPECT_EQ(0, VSIFSeekL(fp, 4, SEEK_SET));
  EXPECT_EQ(4, VSIFTellL(fp));
  // Read 2 bytes purely from the unmapped zero hole [4..6).
  memset(buf, 'X', sizeof(buf));
  EXPECT_EQ(2, VSIFReadL(buf, 1, 2, fp));
  EXPECT_EQ('\0', buf[0]);
  EXPECT_EQ('\0', buf[1]);

  EXPECT_EQ(0, VSIFSeekL(fp, 2, SEEK_CUR));
  EXPECT_EQ(8, VSIFTellL(fp));
  EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_END));
  EXPECT_EQ(14, VSIFTellL(fp));
  errno = 0;
  EXPECT_EQ(-1, fp->Seek(0, 9999));
  EXPECT_EQ(EINVAL, errno);

  EXPECT_EQ(0, VSIFCloseL(fp));
}

TEST(CplVsilSparsefileTest, ErrorPathsMissingSubfileShortReadAndRecursion) {
  const std::string short_subfile = "/vsimem/sparse_short.bin";
  autotest2::VsiMemTempWrapper short_wrap(short_subfile, "123");

  // XML with explicit <Length>30</Length>:
  // - [0..10): SubfileRegion pointing to non-existent file
  // - [10..20): SubfileRegion pointing to 3-byte file (triggers short read)
  const std::string xml_content = R"xml(<VSISparseFile>
  <Length>30</Length>
  <SubfileRegion>
    <Filename>/vsimem/non_existent_region.bin</Filename>
    <DestinationOffset>0</DestinationOffset>
    <SourceOffset>0</SourceOffset>
    <RegionLength>10</RegionLength>
  </SubfileRegion>
  <SubfileRegion>
    <Filename>/vsimem/sparse_short.bin</Filename>
    <DestinationOffset>10</DestinationOffset>
    <SourceOffset>0</SourceOffset>
    <RegionLength>10</RegionLength>
  </SubfileRegion>
</VSISparseFile>)xml";

  const std::string xml_file = "/vsimem/sparse_errors.xml";
  autotest2::VsiMemTempWrapper xml_wrap(xml_file, xml_content);
  const std::string sparse_path = absl::StrCat("/vsisparse/", xml_file);

  VSILFILE* fp = VSIFOpenL(sparse_path.c_str(), "r");
  ASSERT_NE(nullptr, fp);

  char buf[32] = {};
  // First read at 0 tries to open non-existent file (bTriedOpen becomes true).
  EXPECT_EQ(0, VSIFReadL(buf, 1, 5, fp));
  EXPECT_NE(0, VSIFErrorL(fp));
  VSIFClearErrL(fp);
  // Second read at 0 skipsVSIFOpenL because bTriedOpen is already true.
  EXPECT_EQ(0, VSIFReadL(buf, 1, 5, fp));
  EXPECT_NE(0, VSIFErrorL(fp));
  VSIFClearErrL(fp);

  // Read across boundary [8..18) where second region [10..20) has a short read
  // (only 3 bytes in sparse_short.bin instead of 8 requested in recursion).
  EXPECT_EQ(0, VSIFSeekL(fp, 8, SEEK_SET));
  EXPECT_EQ(0, VSIFReadL(buf, 1, 10, fp));
  EXPECT_NE(0, VSIFErrorL(fp));
  VSIFClearErrL(fp);

  // Direct short read inside region [10..20).
  EXPECT_EQ(0, VSIFSeekL(fp, 10, SEEK_SET));
  EXPECT_EQ(3, VSIFReadL(buf, 1, 10, fp));
  EXPECT_NE(0, VSIFErrorL(fp));
  VSIFClearErrL(fp);

  EXPECT_EQ(0, VSIFCloseL(fp));

  // Self-referencing sparse file to test the recursion limit
  // (GetRecCounter() == 32).
  const std::string self_xml = "/vsimem/sparse_self.xml";
  const std::string self_xml_content = R"xml(<VSISparseFile>
  <SubfileRegion>
    <Filename>/vsisparse//vsimem/sparse_self.xml</Filename>
    <DestinationOffset>0</DestinationOffset>
    <SourceOffset>0</SourceOffset>
    <RegionLength>10</RegionLength>
  </SubfileRegion>
</VSISparseFile>)xml";
  autotest2::VsiMemTempWrapper self_wrap(self_xml, self_xml_content);

  VSILFILE* fp_self =
      VSIFOpenL(absl::StrCat("/vsisparse/", self_xml).c_str(), "rb");
  ASSERT_NE(nullptr, fp_self);
  EXPECT_EQ(0, VSIFReadL(buf, 1, 4, fp_self));
  EXPECT_NE(0, VSIFErrorL(fp_self));
  EXPECT_EQ(0, VSIFCloseL(fp_self));
}

}  // namespace
}  // namespace autotest2
