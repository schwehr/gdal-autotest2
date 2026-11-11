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

// Unit tests for //third_party/netcdf/libdispatch/dutil.c (URL/mode helpers,
// backslash/entity/shell escaping, file/temp helpers, split/join, envv sort).
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dutil.c

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <system_error>  // NOLINT
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
#include "third_party/netcdf/include/ncbytes.h"
#include "third_party/netcdf/include/nclist.h"
#include "third_party/netcdf/include/ncuri.h"
#include "third_party/netcdf/include/ncutil.h"
}  // extern "C"

namespace netcdf {
namespace {

char* HeapDup(const char* s) {
  const std::size_t len = std::strlen(s) + 1;
  char* out = static_cast<char*>(std::malloc(len));
  if (out != nullptr) {
    std::memcpy(out, s, len);
  }
  return out;
}

TEST(DutilTest, TestUrlParsesUrlsAndExtractsBasenames) {
  // Upstream ncuriparse(NULL) and ncuriparse("") dereference uninitialized
  // stack memory (`tmp.fraglist`) before `memset(&tmp, 0, sizeof(tmp))`.
  for (const char* non_url : {"/local/path/data.nc", "plain_relative.nc",
                              ":missing_protocol", "file://"}) {
    SCOPED_TRACE(non_url);
    int isfile = -1;
    char* base_raw = nullptr;
    EXPECT_EQ(0, NC__testurl(non_url, &base_raw, &isfile));
    EXPECT_EQ(0, isfile);
    EXPECT_EQ(nullptr, base_raw);
  }

  struct UrlCase {
    const char* name;
    const char* url;
    int expected_isfile;
    const char* expected_base;
  };
  constexpr UrlCase kCases[] = {
      {"file_abs", "file:///dir/sub/dataset.nc", 1, "dataset"},
      {"file_rel_no_slash", "file://relative.nc", 1, "relative"},
      {"https_multi_ext", "https://example.com/a/b/archive.tar.gz", 0,
       "archive.tar"},
      {"https_no_ext", "https://example.com/dir/plain", 0, "plain"},
      {"https_leading_dot", "https://example.com/.hidden", 0, ".hidden"},
      {"https_no_path", "https://example.com", 0, ""},
  };

  for (const UrlCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    char* base_raw = nullptr;
    int isfile = -1;
    EXPECT_EQ(1, NC__testurl(tc.url, &base_raw, &isfile));
    ScopedCString base = MakeScopedCString(base_raw);
    EXPECT_EQ(tc.expected_isfile, isfile);
    ASSERT_NE(nullptr, base.get());
    EXPECT_STREQ(tc.expected_base, base.get());
  }

  // Null optional outputs are tolerated cleanly.
  EXPECT_EQ(1, NC__testurl("https://example.com/file.nc", nullptr, nullptr));
}

TEST(DutilTest, IsLittleEndianMatchesHostByteOrder) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
  EXPECT_EQ(0, NC_isLittleEndian());
#else
  EXPECT_EQ(1, NC_isLittleEndian());
#endif
}

TEST(DutilTest, EscapingAndUnescapingHandleSpecialCharacters) {
  {
    ScopedCString empty = MakeScopedCString(NC_backslashEscape(""));
    ASSERT_NE(nullptr, empty.get());
    EXPECT_STREQ("", empty.get());

    ScopedCString plain = MakeScopedCString(NC_backslashEscape("abc_123"));
    ASSERT_NE(nullptr, plain.get());
    EXPECT_STREQ("abc_123", plain.get());

    ScopedCString esc = MakeScopedCString(NC_backslashEscape("a\\b/c.d@e"));
    ASSERT_NE(nullptr, esc.get());
    EXPECT_STREQ("a\\\\b\\\\c\\\\d\\\\e", esc.get());
  }

  {
    EXPECT_EQ(nullptr, NC_backslashUnescape(nullptr));
    ScopedCString empty = MakeScopedCString(NC_backslashUnescape(""));
    ASSERT_NE(nullptr, empty.get());
    EXPECT_STREQ("", empty.get());

    ScopedCString unesc = MakeScopedCString(NC_backslashUnescape("a\\\\b\\c"));
    ASSERT_NE(nullptr, unesc.get());
    EXPECT_STREQ("a\\bc", unesc.get());
  }

  {
    ScopedCString empty = MakeScopedCString(NC_entityescape(""));
    ASSERT_NE(nullptr, empty.get());
    EXPECT_STREQ("", empty.get());

    ScopedCString ent = MakeScopedCString(NC_entityescape("x&y<z>w\"q'r"));
    ASSERT_NE(nullptr, ent.get());
    EXPECT_STREQ("x&amp;y&lt;z&gt;w&quot;q&apos;r", ent.get());
  }

  {
    EXPECT_EQ(nullptr, NC_shellUnescape(nullptr));
    ScopedCString empty = MakeScopedCString(NC_shellUnescape(""));
    ASSERT_NE(nullptr, empty.get());
    EXPECT_STREQ("", empty.get());

    ScopedCString sh =
        MakeScopedCString(NC_shellUnescape("path\\#mode=nc4\\a\\"));
    ASSERT_NE(nullptr, sh.get());
    EXPECT_STREQ("path#mode=nc4\\a\\", sh.get());
  }
}

TEST(DutilTest, MkTmpCreatesFileAndReportsMissingParentError) {
  TempDir dir("dutil_mktmp");

  const std::string base = (dir.path() / "ut_").native();
  char* tmp_raw = nullptr;
  ASSERT_EQ(NC_NOERR, NC_mktmp(base.c_str(), &tmp_raw));
  ScopedCString tmp = MakeScopedCString(tmp_raw);
  ASSERT_NE(nullptr, tmp.get());
  const absl::Cleanup remove_tmp = [&] {
    std::error_code ec;
    std::filesystem::remove(tmp.get(), ec);
  };
  EXPECT_EQ(std::size_t{0}, std::string(tmp.get()).rfind(base, 0));
  EXPECT_TRUE(std::filesystem::exists(tmp.get()));

  const std::string bad_base =
      (dir.path() / "nonexistent_parent" / "ut_").native();
  char* bad_tmp = reinterpret_cast<char*>(0x1);
  EXPECT_EQ(ENOENT, NC_mktmp(bad_base.c_str(), &bad_tmp));
  EXPECT_EQ(nullptr, bad_tmp);
}

TEST(DutilTest, ReadAndWriteFilesCoverFullPartialTruncatedAndErrors) {
  TempDir dir("dutil_io");

  // Empty file via content == NULL.
  TempFile empty_file("dutil_io/empty.bin");
  ASSERT_EQ(NC_NOERR, NC_writefile(empty_file.c_str(), 123, nullptr));
  {
    NCbytes* buf = ncbytesnew();
    ASSERT_NE(nullptr, buf);
    const absl::Cleanup free_buf = [&] { ncbytesfree(buf); };
    ASSERT_EQ(NC_NOERR, NC_readfile(empty_file.c_str(), buf));
    EXPECT_EQ(0UL, ncbyteslength(buf));
  }

  // Write non-empty content and test full, bounded, exact, and truncated reads.
  TempFile data_file("dutil_io/data.bin");
  std::string payload = "0123456789";
  ASSERT_EQ(NC_NOERR,
            NC_writefile(data_file.c_str(), payload.size(), payload.data()));

  {
    NCbytes* buf = ncbytesnew();
    ASSERT_NE(nullptr, buf);
    const absl::Cleanup free_buf = [&] { ncbytesfree(buf); };

    ASSERT_EQ(NC_NOERR, NC_readfile(data_file.c_str(), buf));
    EXPECT_EQ(10UL, ncbyteslength(buf));
    EXPECT_STREQ("0123456789", ncbytescontents(buf));

    ncbytesclear(buf);
    ASSERT_EQ(NC_NOERR, NC_readfilen(data_file.c_str(), buf, 0));
    EXPECT_EQ(0UL, ncbyteslength(buf));

    ncbytesclear(buf);
    ASSERT_EQ(NC_NOERR, NC_readfilen(data_file.c_str(), buf, 4));
    EXPECT_EQ(4UL, ncbyteslength(buf));
    EXPECT_STREQ("0123", ncbytescontents(buf));

    ncbytesclear(buf);
    ASSERT_EQ(NC_NOERR, NC_readfilen(data_file.c_str(), buf, 10));
    EXPECT_EQ(10UL, ncbyteslength(buf));
    EXPECT_STREQ("0123456789", ncbytescontents(buf));

    ncbytesclear(buf);
    EXPECT_EQ(NC_ETRUNC, NC_readfilen(data_file.c_str(), buf, 25));
    EXPECT_EQ(10UL, ncbyteslength(buf));
  }

  // Missing file read and unwritable parent path write return errno.
  {
    NCbytes* buf = ncbytesnew();
    ASSERT_NE(nullptr, buf);
    const absl::Cleanup free_buf = [&] { ncbytesfree(buf); };
    EXPECT_EQ(ENOENT, NC_readfilen(dir.child("missing.bin").c_str(), buf, -1));
  }
  EXPECT_EQ(ENOENT, NC_writefile(dir.child("no_such_dir/out.bin").c_str(),
                                 payload.size(), payload.data()));

  // Reading from a write-only stream sets ferror and triggers NC_EIO.
  TempFile wronly_file("dutil_io/wronly.bin");
  FILE* wstream = std::fopen(wronly_file.c_str(), "w");
  ASSERT_NE(nullptr, wstream);
  const absl::Cleanup close_wstream = [&] { std::fclose(wstream); };
  NCbytes* err_buf = ncbytesnew();
  ASSERT_NE(nullptr, err_buf);
  const absl::Cleanup free_err_buf = [&] { ncbytesfree(err_buf); };
  EXPECT_EQ(NC_EIO, NC_readfileF(wstream, err_buf, -1));
}

TEST(DutilTest, ModeListTestModeAndAddModeTagManageUriFragments) {
  {
    NClist* list = nullptr;
    ASSERT_EQ(NC_NOERR, NC_getmodelist(nullptr, &list));
    const absl::Cleanup free_list = [&] { nclistfreeall(list); };
    EXPECT_EQ(std::size_t{0}, nclistlength(list));
  }
  {
    NClist* list = nullptr;
    ASSERT_EQ(NC_NOERR, NC_getmodelist("", &list));
    const absl::Cleanup free_list = [&] { nclistfreeall(list); };
    EXPECT_EQ(std::size_t{0}, nclistlength(list));
  }
  {
    NClist* list = nullptr;
    ASSERT_EQ(NC_NOERR, NC_getmodelist("nc4,s3,classic", &list));
    const absl::Cleanup free_list = [&] { nclistfreeall(list); };
    ASSERT_EQ(std::size_t{3}, nclistlength(list));
    EXPECT_STREQ("nc4", static_cast<const char*>(nclistget(list, 0)));
    EXPECT_STREQ("s3", static_cast<const char*>(nclistget(list, 1)));
    EXPECT_STREQ("classic", static_cast<const char*>(nclistget(list, 2)));
  }
  // Use a leading empty delimiter segment (",...") so NC_split_delim rejects
  // on element 0 before allocating elements, avoiding NC_getmodelist's
  // incomplete list cleanup leak on later element errors.
  {
    NClist* bad_list = nullptr;
    EXPECT_EQ(NC_EURL, NC_getmodelist(",,s3", &bad_list));
    EXPECT_EQ(nullptr, bad_list);
  }

  EXPECT_EQ(0, NC_testpathmode("not a url", "s3"));
  EXPECT_EQ(0, NC_testpathmode("https://example.com/a.nc", "s3"));
  EXPECT_EQ(0, NC_testpathmode("https://example.com/a.nc#mode=,,s3", "s3"));
  EXPECT_EQ(1, NC_testpathmode("https://example.com/a.nc#mode=nc4,S3", "s3"));
  EXPECT_EQ(0, NC_testpathmode("https://example.com/a.nc#mode=nc4,S3", "hdf5"));

  NCURI* uri = nullptr;
  ASSERT_EQ(NC_NOERR, ncuriparse("https://example.com/a.nc", &uri));
  const absl::Cleanup free_uri = [&] { ncurifree(uri); };

  // Absent mode fragment gets initialized with first tag.
  EXPECT_EQ(0, NC_testmode(uri, "s3"));
  ASSERT_EQ(NC_NOERR, NC_addmodetag(uri, "s3"));
  EXPECT_EQ(1, NC_testmode(uri, "s3"));

  // Existing mode list appends new tag and deduplicates case-insensitively.
  ASSERT_EQ(NC_NOERR, NC_addmodetag(uri, "nc4"));
  EXPECT_STREQ("s3,nc4", ncurifragmentlookup(uri, "mode"));
  ASSERT_EQ(NC_NOERR, NC_addmodetag(uri, "NC4"));
  EXPECT_STREQ("s3,nc4", ncurifragmentlookup(uri, "mode"));

  // Corrupted existing mode list propagates NC_EURL.
  ASSERT_EQ(NC_NOERR, ncurisetfragmentkey(uri, "mode", ",,broken"));
  EXPECT_EQ(NC_EURL, NC_addmodetag(uri, "extra"));
}

TEST(DutilTest, SplitDelimJoinAndJoinWithFormatSegments) {
  EXPECT_EQ(NC_NOERR, NC_split_delim(nullptr, '/', nullptr));
  EXPECT_EQ(NC_NOERR, NC_split_delim("", '/', nullptr));
  EXPECT_EQ(NC_NOERR, NC_split_delim("a/b", '/', nullptr));

  {
    NClist* segs = nclistnew();
    const absl::Cleanup free_segs = [&] { nclistfreeall(segs); };

    // Leading '/' is stripped and trailing '/' stops at empty tail.
    ASSERT_EQ(NC_NOERR, NC_split_delim("/alpha/beta/", '/', segs));
    ASSERT_EQ(std::size_t{2}, nclistlength(segs));
    EXPECT_STREQ("alpha", static_cast<const char*>(nclistget(segs, 0)));
    EXPECT_STREQ("beta", static_cast<const char*>(nclistget(segs, 1)));

    char* joined_raw = nullptr;
    ASSERT_EQ(NC_NOERR, NC_join(segs, &joined_raw));
    ScopedCString joined = MakeScopedCString(joined_raw);
    EXPECT_STREQ("/alpha/beta", joined.get());

    char* joined_with_raw = nullptr;
    ASSERT_EQ(NC_NOERR, NC_joinwith(segs, ":", "[", "]", &joined_with_raw));
    ScopedCString joined_with = MakeScopedCString(joined_with_raw);
    EXPECT_STREQ("[alpha:beta]", joined_with.get());
    EXPECT_EQ(NC_NOERR, NC_joinwith(segs, "/", nullptr, nullptr, nullptr));
  }

  // Segment already starting with separator avoids duplicate separator.
  {
    NClist* segs = nclistnew();
    const absl::Cleanup free_segs = [&] { nclistfreeall(segs); };
    nclistpush(segs, HeapDup("first"));
    nclistpush(segs, HeapDup("/second"));

    char* joined_raw = nullptr;
    ASSERT_EQ(NC_NOERR, NC_joinwith(segs, "/", nullptr, nullptr, &joined_raw));
    ScopedCString joined = MakeScopedCString(joined_raw);
    EXPECT_STREQ("first/second", joined.get());
  }

  // Empty segments list joins to root "/", and null segments yields NC_EINVAL.
  {
    NClist* empty = nclistnew();
    const absl::Cleanup free_empty = [&] { nclistfreeall(empty); };
    char* empty_join_raw = nullptr;
    ASSERT_EQ(NC_NOERR, NC_join(empty, &empty_join_raw));
    ScopedCString empty_join = MakeScopedCString(empty_join_raw);
    EXPECT_STREQ("/", empty_join.get());
  }
  EXPECT_EQ(NC_EINVAL, NC_joinwith(nullptr, "/", nullptr, nullptr, nullptr));

  // Consecutive delimiters fail with NC_EURL.
  {
    NClist* bad_segs = nclistnew();
    const absl::Cleanup free_bad = [&] { nclistfreeall(bad_segs); };
    EXPECT_EQ(NC_EURL, NC_split_delim("a//b", '/', bad_segs));
  }
}

TEST(DutilTest, SortAndFreeEnvvHandleEmptySingleMultiAndNullSlots) {
  NC_sortenvv(0, nullptr);
  char single[] = "only";
  char* one[] = {single};
  NC_sortenvv(1, one);
  EXPECT_STREQ("only", one[0]);

  char s0[] = "delta";
  char s1[] = "alpha";
  char s2[] = "charlie";
  char s3[] = "bravo";
  char* items[] = {s0, s1, s2, s3};
  NC_sortenvv(4, items);
  std::vector<std::string> sorted(items, items + 4);
  EXPECT_THAT(sorted,
              ::testing::ElementsAre("alpha", "bravo", "charlie", "delta"));

  NC_freeenvv(0, nullptr);
  char** heap_envv = static_cast<char**>(std::calloc(3, sizeof(char*)));
  ASSERT_NE(nullptr, heap_envv);
  heap_envv[0] = HeapDup("first");
  heap_envv[1] = nullptr;
  heap_envv[2] = HeapDup("third");
  NC_freeenvv(3, heap_envv);
}

}  // namespace
}  // namespace netcdf
