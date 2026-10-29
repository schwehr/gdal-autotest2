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
// Tests for GDAL's abstract archive virtual file system base class.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil_abstract_archive.cpp

#include <sys/stat.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

struct FakeEntry {
  std::string name;
  GUIntBig size;
  GIntBig mtime;
};

class FakeOffset : public VSIArchiveEntryFileOffset {
 public:
  explicit FakeOffset(size_t idx) : idx_(idx) {}
  ~FakeOffset() override = default;
  size_t idx() const { return idx_; }

 private:
  size_t idx_;
};

class FakeArchiveReader : public VSIArchiveReader {
 public:
  FakeArchiveReader(std::vector<FakeEntry> entries, bool fail_first,
                    bool fail_goto_offset)
      : entries_(std::move(entries)),
        fail_first_(fail_first),
        fail_goto_offset_(fail_goto_offset) {}

  ~FakeArchiveReader() override = default;

  int GotoFirstFile() override {
    if (fail_first_ || entries_.empty()) {
      return FALSE;
    }
    cur_ = 0;
    return TRUE;
  }

  int GotoNextFile() override {
    if (cur_ + 1 >= entries_.size()) {
      return FALSE;
    }
    ++cur_;
    return TRUE;
  }

  VSIArchiveEntryFileOffset* GetFileOffset() override {
    return new FakeOffset(cur_);
  }

  GUIntBig GetFileSize() override { return entries_[cur_].size; }

  CPLString GetFileName() override { return entries_[cur_].name; }

  GIntBig GetModifiedTime() override { return entries_[cur_].mtime; }

  int GotoFileOffset(VSIArchiveEntryFileOffset* pOffset) override {
    if (fail_goto_offset_ || pOffset == nullptr) {
      return FALSE;
    }
    auto* fake_off = static_cast<FakeOffset*>(pOffset);
    if (fake_off->idx() >= entries_.size()) {
      return FALSE;
    }
    cur_ = fake_off->idx();
    return TRUE;
  }

 private:
  std::vector<FakeEntry> entries_;
  size_t cur_ = 0;
  bool fail_first_ = false;
  bool fail_goto_offset_ = false;
};

class FakeArchiveFSHandler : public VSIArchiveFilesystemHandler {
 public:
  FakeArchiveFSHandler() = default;
  ~FakeArchiveFSHandler() override = default;

  const char* GetPrefix() const override { return "/vsifake"; }

  std::vector<CPLString> GetExtensions() const override {
    return {".fzip", ".ftar"};
  }

  std::unique_ptr<VSIArchiveReader> CreateReader(
      const char* pszArchiveFileName) override {
    if (return_null_reader_) {
      return nullptr;
    }
    return std::make_unique<FakeArchiveReader>(entries_, fail_first_,
                                               fail_goto_offset_);
  }

  VSIVirtualHandleUniquePtr Open(const char* pszFilename, const char* pszAccess,
                                 bool bSetError,
                                 CSLConstList papszOptions) override {
    return nullptr;
  }

  void SetEntries(std::vector<FakeEntry> entries) {
    entries_ = std::move(entries);
  }
  void SetReturnNullReader(bool val) { return_null_reader_ = val; }
  void SetFailFirst(bool val) { fail_first_ = val; }
  void SetFailGotoOffset(bool val) { fail_goto_offset_ = val; }

 private:
  std::vector<FakeEntry> entries_;
  bool return_null_reader_ = false;
  bool fail_first_ = false;
  bool fail_goto_offset_ = false;
};

TEST(CplVsilAbstractArchiveTest, GetContentAndStrippedFilenames) {
  FakeArchiveFSHandler handler;
  // Non-existent archive file -> VSIStatL != 0.
  EXPECT_EQ(nullptr,
            handler.GetContentOfArchive("/vsimem/does_not_exist.fzip"));

  autotest2::VsiMemTempWrapper archive("/vsimem/test1.fzip", "dummy content");

  // Null reader.
  handler.SetReturnNullReader(true);
  EXPECT_EQ(nullptr, handler.GetContentOfArchive("/vsimem/test1.fzip"));
  handler.SetReturnNullReader(false);

  // GotoFirstFile fails.
  handler.SetFailFirst(true);
  EXPECT_EQ(nullptr, handler.GetContentOfArchive("/vsimem/test1.fzip"));
  handler.SetFailFirst(false);

  // Various filenames: "./", "./a\\b/c.txt", "/abs", "a//b", "dir/", duplicate.
  handler.SetEntries({
      {"./", 0, 100},
      {"/abs/file.txt", 10, 100},
      {"bad//slash.txt", 10, 100},
      {"./sub\\nested/file1.txt", 42, 1000},
      {"./sub/nested/file1.txt", 42, 1000},  // duplicate after stripping
      {"explicit_dir/", 0, 2000},
      {"root_file.txt", 15, 3000},
  });

  const VSIArchiveContent* content =
      handler.GetContentOfArchive("/vsimem/test1.fzip");
  ASSERT_NE(nullptr, content);
  // Cached lookup should return the exact same pointer.
  EXPECT_EQ(content, handler.GetContentOfArchive("/vsimem/test1.fzip"));

  // Modify archive size to invalidate cached content.
  {
    VSILFILE* fp = VSIFOpenL("/vsimem/test1.fzip", "wb");
    ASSERT_NE(nullptr, fp);
    ASSERT_EQ(1, VSIFWriteL("new longer dummy content", 24, 1, fp));
    ASSERT_EQ(0, VSIFCloseL(fp));
  }
  const VSIArchiveContent* content2 =
      handler.GetContentOfArchive("/vsimem/test1.fzip");
  ASSERT_NE(nullptr, content2);
}

TEST(CplVsilAbstractArchiveTest, SplitFilenameVariants) {
  FakeArchiveFSHandler handler;
  handler.SetEntries({{"dir/a.txt", 5, 100}});
  autotest2::VsiMemTempWrapper archive("/vsimem/split.fzip", "data");

  CPLString file_in_archive;

  // Exact prefix match returns nullptr.
  EXPECT_EQ(nullptr,
            handler.SplitFilename("/vsifake", file_in_archive, true, false));

  // Unbalanced curly braces returns nullptr.
  EXPECT_EQ(nullptr, handler.SplitFilename("/vsifake/{/vsimem/split.fzip/a.txt",
                                           file_in_archive, true, false));

  // Extended syntax with bCheckMainFileExists = false.
  char* res = handler.SplitFilename(
      "/vsifake/{/vsimem/nonexistent_ok.fzip}/a/../b/foo/c/../d/",
      file_in_archive, false, false);
  ASSERT_NE(nullptr, res);
  EXPECT_STREQ("/vsimem/nonexistent_ok.fzip", res);
  EXPECT_EQ("b/foo/d", file_in_archive);
  CPLFree(res);

  // Extended syntax with empty file_in_archive.
  res = handler.SplitFilename("/vsifake/{/vsimem/split.fzip}", file_in_archive,
                              true, true);
  ASSERT_NE(nullptr, res);
  EXPECT_STREQ("/vsimem/split.fzip", res);
  EXPECT_EQ("", file_in_archive);
  CPLFree(res);

  // Extended syntax with invalid char after '}'.
  EXPECT_EQ(nullptr, handler.SplitFilename("/vsifake/{/vsimem/split.fzip}bad",
                                           file_in_archive, true, false));

  // Extended syntax where file does not exist and bCheckMainFileExists = true.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr,
              handler.SplitFilename("/vsifake/{/vsimem/missing.fzip}/a.txt",
                                    file_in_archive, true, true));
  }

  // Prime cache so oFileList.find(archiveFilename) succeeds in SplitFilename.
  ASSERT_NE(nullptr, handler.GetContentOfArchive("/vsimem/split.fzip"));

  // Extended syntax hitting oFileList cache.
  res = handler.SplitFilename("/vsifake/{/vsimem/split.fzip}/dir/a.txt\\",
                              file_in_archive, true, false);
  ASSERT_NE(nullptr, res);
  EXPECT_STREQ("/vsimem/split.fzip", res);
  EXPECT_EQ("dir/a.txt", file_in_archive);
  CPLFree(res);

  // Non-extended syntax: natural chaining "/vsifake/vsimem/..." vs
  // "//vsimem/..."
  res = handler.SplitFilename("/vsifake/vsimem/split.fzip/dir/a.txt/",
                              file_in_archive, true, false);
  ASSERT_NE(nullptr, res);
  EXPECT_STREQ("/vsimem/split.fzip", res);
  EXPECT_EQ("dir/a.txt", file_in_archive);
  CPLFree(res);

  // Non-extended syntax without in-archive file and uncached file.
  autotest2::VsiMemTempWrapper archive2("/vsimem/split2.ftar", "data");
  res = handler.SplitFilename("/vsifake//vsimem/split2.ftar", file_in_archive,
                              true, false);
  ASSERT_NE(nullptr, res);
  EXPECT_STREQ("/vsimem/split2.ftar", res);
  EXPECT_EQ("", file_in_archive);
  CPLFree(res);

  // Non-extended syntax with bCheckMainFileExists = false.
  res = handler.SplitFilename("/vsifake//vsimem/no_such.fzip/x",
                              file_in_archive, false, false);
  ASSERT_NE(nullptr, res);
  EXPECT_STREQ("/vsimem/no_such.fzip", res);
  EXPECT_EQ("x", file_in_archive);
  CPLFree(res);

  // Exceed nAttempts == 5 threshold with non-existent repeated extensions.
  EXPECT_EQ(nullptr,
            handler.SplitFilename(
                "/vsifake//vsimem/a.fzip/b.fzip/c.fzip/d.fzip/e.fzip/f.fzip",
                file_in_archive, true, false));

  // Exceed recursion depth (*pnCounter == 3) by installing handler temporarily.
  VSIFileManager::InstallHandler("/vsifake", &handler);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        nullptr,
        handler.SplitFilename(
            "/vsifake/vsifake/vsifake/vsifake/vsimem/d1.fzip/d2.fzip/d3.fzip/"
            "d4.fzip/a.txt",
            file_in_archive, true, false));
  }
  VSIFileManager::RemoveHandler("/vsifake");
}

TEST(CplVsilAbstractArchiveTest, OpenArchiveFileVariants) {
  FakeArchiveFSHandler handler;
  autotest2::VsiMemTempWrapper archive("/vsimem/open.fzip", "data");

  // Null reader.
  handler.SetReturnNullReader(true);
  EXPECT_EQ(nullptr, handler.OpenArchiveFile("/vsimem/open.fzip", nullptr));
  handler.SetReturnNullReader(false);

  // GotoFirstFile fails when fileInArchiveName is empty.
  handler.SetFailFirst(true);
  EXPECT_EQ(nullptr, handler.OpenArchiveFile("/vsimem/open.fzip", ""));
  // GotoFirstFile fails when fileInArchiveName is non-empty and uncached.
  EXPECT_EQ(nullptr, handler.OpenArchiveFile("/vsimem/open.fzip", "a.txt"));
  handler.SetFailFirst(false);

  // Only a directory in archive when fileInArchiveName is empty.
  handler.SetEntries({{"subdir/", 0, 100}});
  EXPECT_EQ(nullptr, handler.OpenArchiveFile("/vsimem/open.fzip", ""));

  // Leading subdir followed by single file when fileInArchiveName is empty.
  handler.SetEntries({{"subdir/", 0, 100}, {"subdir/only.txt", 12, 101}});
  auto reader = handler.OpenArchiveFile("/vsimem/open.fzip", "");
  ASSERT_NE(nullptr, reader);
  EXPECT_EQ("subdir/only.txt", reader->GetFileName());

  // Multiple files in archive when fileInArchiveName is empty -> error.
  handler.SetEntries({{"file1.txt", 10, 100}, {"file2.txt", 20, 200}});
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, handler.OpenArchiveFile("/vsimem/open.fzip", ""));
  }

  // Uncached optimization: first entry matches fileInArchiveName!
  autotest2::VsiMemTempWrapper archive_first("/vsimem/first.fzip", "data");
  handler.SetEntries({{"dir_first/", 0, 100}, {"second.txt", 20, 200}});
  // First entry is a directory -> returns nullptr.
  EXPECT_EQ(nullptr,
            handler.OpenArchiveFile("/vsimem/first.fzip", "dir_first"));

  handler.SetEntries({{"first.txt", 10, 100}, {"second.txt", 20, 200}});
  reader = handler.OpenArchiveFile("/vsimem/first.fzip", "first.txt");
  ASSERT_NE(nullptr, reader);
  EXPECT_EQ("first.txt", reader->GetFileName());

  // Second entry requires FindFileInArchive + GotoFileOffset.
  reader = handler.OpenArchiveFile("/vsimem/first.fzip", "second.txt");
  ASSERT_NE(nullptr, reader);
  EXPECT_EQ("second.txt", reader->GetFileName());

  // GotoFileOffset failure.
  handler.SetFailGotoOffset(true);
  EXPECT_EQ(nullptr,
            handler.OpenArchiveFile("/vsimem/first.fzip", "second.txt"));
  handler.SetFailGotoOffset(false);

  // Non-existent entry in archive.
  EXPECT_EQ(nullptr,
            handler.OpenArchiveFile("/vsimem/first.fzip", "missing.txt"));
}

TEST(CplVsilAbstractArchiveTest, StatReadDirIsLocalIsArchive) {
  FakeArchiveFSHandler handler;
  autotest2::VsiMemTempWrapper archive("/vsimem/stat.fzip", "data");

  VSIStatBufL stat_buf;
  // Invalid filename.
  EXPECT_EQ(-1, handler.Stat("/vsifake", &stat_buf, 0));

  // Empty archive (only subdir/) with emptyosFileInArchive.
  handler.SetEntries({{"subdir/", 0, 100}});
  EXPECT_EQ(-1, handler.Stat("/vsifake//vsimem/stat.fzip", &stat_buf, 0));

  // Single file in archive (after leading subdir/) with empty osFileInArchive.
  handler.SetEntries({{"subdir/", 0, 100}, {"subdir/single.txt", 77, 500}});
  EXPECT_EQ(0, handler.Stat("/vsifake//vsimem/stat.fzip", &stat_buf, 0));
  EXPECT_EQ(77, stat_buf.st_size);
  EXPECT_TRUE(VSI_ISREG(stat_buf.st_mode));

  // Multiple files in archive with empty osFileInArchive -> treated as dir.
  handler.SetEntries({{"subdir/a.txt", 10, 200},
                      {"subdir/b.txt", 20, 300},
                      {"top.txt", 5, 400}});
  EXPECT_EQ(0, handler.Stat("/vsifake//vsimem/stat.fzip", &stat_buf, 0));
  EXPECT_TRUE(VSI_ISDIR(stat_buf.st_mode));

  // Stat specific file and directory inside archive.
  EXPECT_EQ(
      0, handler.Stat("/vsifake//vsimem/stat.fzip/subdir/a.txt", &stat_buf, 0));
  EXPECT_EQ(10, stat_buf.st_size);
  EXPECT_TRUE(VSI_ISREG(stat_buf.st_mode));

  EXPECT_EQ(0, handler.Stat("/vsifake//vsimem/stat.fzip/subdir", &stat_buf, 0));
  EXPECT_TRUE(VSI_ISDIR(stat_buf.st_mode));

  // ReadDirEx tests.
  EXPECT_EQ(nullptr, handler.ReadDirEx("/vsifake", 0));

  // ReadDirEx when GetContentOfArchive returns nullptr.
  autotest2::VsiMemTempWrapper bad_archive("/vsimem/bad.fzip", "data");
  handler.SetFailFirst(true);
  EXPECT_EQ(nullptr, handler.ReadDirEx("/vsifake//vsimem/bad.fzip", 0));
  handler.SetFailFirst(false);

  // ReadDirEx on root of stat.fzip.
  CPLStringList root_dir(handler.ReadDirEx("/vsifake//vsimem/stat.fzip", 0),
                         TRUE);
  EXPECT_EQ(2, root_dir.Count());

  // ReadDirEx on subdir with nMaxFiles = 1.
  CPLStringList sub_dir(
      handler.ReadDirEx("/vsifake//vsimem/stat.fzip/subdir", 1), TRUE);
  EXPECT_EQ(2, sub_dir.Count());  // Breaks when Count() > nMaxFiles (2 > 1).

  // ReadDirEx on non-existent subdir inside archive.
  EXPECT_EQ(nullptr,
            handler.ReadDirEx("/vsifake//vsimem/stat.fzip/no_such_dir", 0));

  // IsLocal and IsArchive.
  // Note: VSIArchiveFilesystemHandler::IsArchive leaks the char* returned by
  // SplitFilename when SplitFilename != nullptr, so test paths where
  // SplitFilename returns nullptr.
  EXPECT_FALSE(handler.IsLocal("/other/path"));
  EXPECT_TRUE(handler.IsLocal("/vsifake/local/file.fzip"));

  EXPECT_FALSE(handler.IsArchive("/other/path"));
  EXPECT_FALSE(handler.IsArchive("/vsifake"));

  EXPECT_FALSE(handler.SupportsSequentialWrite("/vsifake/a", false));
  EXPECT_FALSE(handler.SupportsRandomWrite("/vsifake/a", false));
}

}  // namespace
}  // namespace autotest2
