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

// Unit tests for //third_party/netcdf/libdispatch/ncuri.c (NCURI parser,
// builder, query/fragment parameter handling, cloning, and percent-encoding).
//
// Covers standard and file: URI parsing, bracketed DAP2 prefix parameters,
// query/fragment lookup and deduplication, mutators, rebuild/build flags,
// deep cloning, and URL percent-encoding/decoding helpers.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/ncuri.c

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/include/netcdf.h"

extern "C" {
#include "third_party/netcdf/include/nclist.h"
#include "third_party/netcdf/include/ncuri.h"
char* ncuriunescape(const char* s);
}  // extern "C"

namespace netcdf {
namespace {

struct NcUriDeleter {
  void operator()(NCURI* uri) const { ncurifree(uri); }
};

using ScopedNcUri = std::unique_ptr<NCURI, NcUriDeleter>;

char* DupCString(std::string_view s) {
  auto* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out != nullptr) {
    if (!s.empty()) {
      std::memcpy(out, s.data(), s.size());
    }
    out[s.size()] = '\0';
  }
  return out;
}

TEST(NcuriTest, ParsesStandardSchemesHostPortUserPwdPathQueryAndFragment) {
  // Full URI with leading/trailing spaces, doubled backslashes, percent-encoded
  // credentials, numeric port, escaped '?'/'#'/'/'/'&', query, and fragment.
  constexpr char kRawFull[] =
      "   https://user%401:p%3Ass%202@data.example.test:8443/"
      "dir\\\\sub/f\\?1\\#2?a=x\\&y&flag#c=u\\&v&mode   ";
  NCURI* raw = nullptr;
  ASSERT_EQ(NC_NOERR, ncuriparse(kRawFull, &raw));
  ScopedNcUri full(raw);
  ASSERT_NE(nullptr, full.get());

  EXPECT_STREQ(kRawFull, full->uri);
  EXPECT_STREQ("https", full->protocol);
  EXPECT_STREQ("user@1", full->user);
  EXPECT_STREQ("p:ss 2", full->password);
  EXPECT_STREQ("data.example.test", full->host);
  EXPECT_STREQ("8443", full->port);
  EXPECT_STREQ("/dir\\sub/f\\?1\\#2", full->path);
  EXPECT_STREQ("a=x\\&y&flag", full->query);
  EXPECT_STREQ("c=u\\&v&mode", full->fragment);
  EXPECT_STREQ("x\\&y", ncuriquerylookup(full.get(), "a"));
  EXPECT_STREQ("", ncuriquerylookup(full.get(), "flag"));
  EXPECT_STREQ("u\\&v", ncurifragmentlookup(full.get(), "c"));
  EXPECT_STREQ("", ncurifragmentlookup(full.get(), "mode"));

  // Host without path and dash-containing port.
  raw = nullptr;
  ASSERT_EQ(NC_NOERR, ncuriparse("http://host.example.test:-1", &raw));
  ScopedNcUri no_path(raw);
  ASSERT_NE(nullptr, no_path.get());
  EXPECT_STREQ("http", no_path->protocol);
  EXPECT_STREQ("host.example.test", no_path->host);
  EXPECT_STREQ("-1", no_path->port);
  EXPECT_EQ(nullptr, no_path->path);
  EXPECT_EQ(nullptr, no_path->query);
  EXPECT_EQ(nullptr, no_path->fragment);

  // Host-only without port or path, and query-only without fragment.
  raw = nullptr;
  ASSERT_EQ(NC_NOERR, ncuriparse("http://host.only?k=1", &raw));
  ScopedNcUri query_only(raw);
  ASSERT_NE(nullptr, query_only.get());
  EXPECT_STREQ("host.only", query_only->host);
  EXPECT_EQ(nullptr, query_only->port);
  EXPECT_EQ(nullptr, query_only->path);
  EXPECT_STREQ("k=1", query_only->query);
  EXPECT_EQ(nullptr, query_only->fragment);

  // Empty host with path, escaped slash in host, and fragment-only.
  raw = nullptr;
  ASSERT_EQ(NC_NOERR, ncuriparse("http:///only/path#frag=val", &raw));
  ScopedNcUri empty_host(raw);
  ASSERT_NE(nullptr, empty_host.get());
  EXPECT_EQ(nullptr, empty_host->host);
  EXPECT_STREQ("/only/path", empty_host->path);
  EXPECT_EQ(nullptr, empty_host->query);
  EXPECT_STREQ("frag=val", empty_host->fragment);

  raw = nullptr;
  ASSERT_EQ(NC_NOERR, ncuriparse("http://host\\/part/tail", &raw));
  ScopedNcUri escaped_host(raw);
  ASSERT_NE(nullptr, escaped_host.get());
  EXPECT_STREQ("host\\/part", escaped_host->host);
  EXPECT_STREQ("/tail", escaped_host->path);

  // Empty host and empty path, plus empty query/fragment delimiters.
  raw = nullptr;
  ASSERT_EQ(NC_NOERR, ncuriparse("http://", &raw));
  ScopedNcUri bare_proto(raw);
  ASSERT_NE(nullptr, bare_proto.get());
  EXPECT_STREQ("http", bare_proto->protocol);
  EXPECT_EQ(nullptr, bare_proto->host);
  EXPECT_EQ(nullptr, bare_proto->path);

  raw = nullptr;
  ASSERT_EQ(NC_NOERR, ncuriparse("http://h/p?#", &raw));
  ScopedNcUri empty_qf(raw);
  ASSERT_NE(nullptr, empty_qf.get());
  EXPECT_EQ(nullptr, empty_qf->query);
  EXPECT_EQ(nullptr, empty_qf->fragment);

  raw = nullptr;
  ASSERT_EQ(NC_NOERR, ncuriparse("http://h/p?", &raw));
  ScopedNcUri trailing_q(raw);
  ASSERT_NE(nullptr, trailing_q.get());
  EXPECT_EQ(nullptr, trailing_q->query);
}

TEST(NcuriTest, ParsesAllValidFileUriForms) {
  struct FileCase {
    const char* uri;
    const char* expected_path;
    const char* expected_query;
    const char* expected_frag;
  };
  constexpr FileCase kCases[] = {
      {"file:C:/data/one.nc", "C:/data/one.nc", nullptr, nullptr},
      {"file:d:\\\\data\\\\two.nc", "d:\\data\\two.nc", nullptr, nullptr},
      {"file:/abs/three.nc", "/abs/three.nc", nullptr, nullptr},
      {"file://E:/win/four.nc", "E:/win/four.nc", nullptr, nullptr},
      {"file:///abs/five.nc?v=1#opt=on", "/abs/five.nc", "v=1", "opt=on"},
      {"file://rel/six.nc", "rel/six.nc", nullptr, nullptr},
  };

  for (const FileCase& tc : kCases) {
    SCOPED_TRACE(tc.uri);
    NCURI* raw = nullptr;
    ASSERT_EQ(NC_NOERR, ncuriparse(tc.uri, &raw));
    ScopedNcUri uri(raw);
    ASSERT_NE(nullptr, uri.get());
    EXPECT_STREQ("file", uri->protocol);
    EXPECT_EQ(nullptr, uri->host);
    EXPECT_EQ(nullptr, uri->port);
    EXPECT_EQ(nullptr, uri->user);
    EXPECT_EQ(nullptr, uri->password);
    EXPECT_STREQ(tc.expected_path, uri->path);
    if (tc.expected_query == nullptr) {
      EXPECT_EQ(nullptr, uri->query);
    } else {
      EXPECT_STREQ(tc.expected_query, uri->query);
    }
    if (tc.expected_frag == nullptr) {
      EXPECT_EQ(nullptr, uri->fragment);
    } else {
      EXPECT_STREQ(tc.expected_frag, uri->fragment);
    }
  }
}

TEST(NcuriTest, ParsesPrefixParametersMergesWithFragmentAndDeduplicates) {
  // Keep distinct values ("a=1" vs "a=9") in the parent process: when both key
  // and value match case-insensitively, upstream removedups() calls
  // nclistremove() without freeing the removed strings. Identical-pair
  // deduplication is exercised in the isolated child test below.
  NCURI* raw = nullptr;
  ASSERT_EQ(
      NC_NOERR,
      ncuriparse("[a=1][b=x\\]y][flag]https://srv.test/ds#a=9&c=3", &raw));
  ScopedNcUri uri(raw);
  ASSERT_NE(nullptr, uri.get());

  // Distinct-valued ("a", "9") is preserved and first match wins on lookup.
  EXPECT_STREQ("1", ncurifragmentlookup(uri.get(), "a"));
  EXPECT_STREQ("x\\]y", ncurifragmentlookup(uri.get(), "b"));
  EXPECT_STREQ("", ncurifragmentlookup(uri.get(), "flag"));
  EXPECT_STREQ("3", ncurifragmentlookup(uri.get(), "c"));
  EXPECT_STREQ("a=1&b=x\\]y&flag&a=9&c=3", uri->fragment);

  // Single-pair fragment hits removedups() nclistlength <= 2 fast return.
  raw = nullptr;
  ASSERT_EQ(NC_NOERR, ncuriparse("http://srv.test/ds#only=1", &raw));
  ScopedNcUri single_frag(raw);
  ASSERT_NE(nullptr, single_frag.get());
  EXPECT_STREQ("1", ncurifragmentlookup(single_frag.get(), "only"));
}

TEST(NcuriTest, RejectsMalformedUrisWithNcEurl) {
  constexpr const char* kBadUris[] = {
      "no_colon_at_all",
      "://host/path",
      "[a=1]",
      "[unclosedhttp://host/path",
      "[a=1][unclosedhttp://host/path",
      "http:",
      "http:host/path",
      "http:/host/path",
      "file:",
      "file:/",
      "file://",
      "file:///",
      "file:////too_many_slashes",
      "file:relative_no_drive",
      "file:1:/not_drive_letter",
      "file://a",
      "http://@host/path",
      "http://useronly@host/path",
      "http://:secret@host/path",
      "http://alice:@host/path",
      "http://:8080/path",
      "http://alice:secret@:8080/path",
      "http://host:/path",
      "http://host:80a0/path",
      // Exercises error cleanup when fraglist & querylist are already built.
      "http://host:badport?q=1#f=2",
  };

  for (const char* bad : kBadUris) {
    SCOPED_TRACE(bad);
    NCURI* out = nullptr;
    EXPECT_EQ(NC_EURL, ncuriparse(bad, &out));
    EXPECT_EQ(nullptr, out);
  }
}

TEST(NcuriTest, MutatorsRebuildBuildFlagsAndNullSafeFree) {
  ncurifree(nullptr);

  NCURI* raw = nullptr;
  ASSERT_EQ(NC_NOERR,
            ncuriparse("http://alice:sec@old.test:8080/dir name/[x]?q=a b#f=1",
                       &raw));
  ScopedNcUri uri(raw);
  ASSERT_NE(nullptr, uri.get());

  // Exercise all flag combinations on the parsed URI.
  ScopedCString host_only =
      MakeScopedCString(ncuribuild(uri.get(), nullptr, nullptr, 0));
  EXPECT_STREQ("http://old.test:8080", host_only.get());

  ScopedCString base_only =
      MakeScopedCString(ncuribuild(uri.get(), nullptr, nullptr, NCURIBASE));
  EXPECT_STREQ("http://alice:sec@old.test:8080/dir name/[x]", base_only.get());

  ScopedCString svc =
      MakeScopedCString(ncuribuild(uri.get(), nullptr, ".dds", NCURISVC));
  EXPECT_STREQ("http://alice:sec@old.test:8080/dir name/[x].dds?q=a b",
               svc.get());

  ScopedCString all_enc = MakeScopedCString(
      ncuribuild(uri.get(), "[pre=1]", nullptr, NCURIALL | NCURIENCODE));
  EXPECT_STREQ(
      "[pre=1]http://alice:sec@old.test:8080/dir%20name/%5bx%5d?q=a%20b#f=1",
      all_enc.get());

  // Mutate protocol, host, path, query, and fragments, then rebuild uri->uri.
  // Keep distinct key/value pairs here because upstream removedups() leaks
  // duplicate pair strings via nclistremove() without freeing them; duplicate
  // removal with ncurisetquery()/ncurisetfragments() is covered in the isolated
  // child test below.
  EXPECT_EQ(NC_NOERR, ncurisetprotocol(uri.get(), "https"));
  EXPECT_EQ(NC_NOERR, ncurisethost(uri.get(), "new.test"));
  EXPECT_EQ(NC_NOERR, ncurisetpath(uri.get(), "/new/path"));
  EXPECT_EQ(NC_NOERR, ncurisetquery(uri.get(), "k=v&m=2"));
  EXPECT_STREQ("v", ncuriquerylookup(uri.get(), "k"));
  EXPECT_STREQ("2", ncuriquerylookup(uri.get(), "m"));
  EXPECT_STREQ("k=v&m=2", uri->query);

  EXPECT_EQ(NC_NOERR, ncurisetfragments(uri.get(), "f1=1&f2"));
  EXPECT_STREQ("1", ncurifragmentlookup(uri.get(), "f1"));
  EXPECT_STREQ("", ncurifragmentlookup(uri.get(), "f2"));
  EXPECT_STREQ("f1=1&f2", uri->fragment);

  EXPECT_EQ(NC_NOERR, ncurirebuild(uri.get()));
  EXPECT_STREQ("https://alice:sec@new.test:8080/new/path?k=v&m=2#f1=1&f2",
               uri->uri);

  // Clearing query and fragments with empty string and nullptr.
  EXPECT_EQ(NC_NOERR, ncurisetquery(uri.get(), ""));
  EXPECT_EQ(nullptr, uri->query);
  EXPECT_EQ(nullptr, uri->querylist);
  EXPECT_EQ(NC_NOERR, ncurisetquery(uri.get(), "x=1"));
  EXPECT_NE(nullptr, uri->query);
  EXPECT_EQ(NC_NOERR, ncurisetquery(uri.get(), nullptr));
  EXPECT_EQ(nullptr, uri->query);
  EXPECT_EQ(nullptr, uri->querylist);

  EXPECT_EQ(NC_NOERR, ncurisetfragments(uri.get(), ""));
  EXPECT_EQ(nullptr, uri->fragment);
  EXPECT_EQ(nullptr, uri->fraglist);
  EXPECT_EQ(NC_NOERR, ncurisetfragments(uri.get(), "y=2"));
  EXPECT_NE(nullptr, uri->fragment);
  EXPECT_EQ(NC_NOERR, ncurisetfragments(uri.get(), nullptr));
  EXPECT_EQ(nullptr, uri->fragment);
  EXPECT_EQ(nullptr, uri->fraglist);

  // Building when query and fragment are null still respects flags cleanly.
  ScopedCString cleared_all =
      MakeScopedCString(ncuribuild(uri.get(), nullptr, nullptr, NCURIALL));
  EXPECT_STREQ("https://alice:sec@new.test:8080/new/path", cleared_all.get());

  // Building a URI with null path inserts "/" when NCURIPATH is set, and
  // encodes special characters in user/password.
  raw = nullptr;
  ASSERT_EQ(NC_NOERR, ncuriparse("https://u%401:p%3A%202@host.test", &raw));
  ScopedNcUri no_path_uri(raw);
  ASSERT_NE(nullptr, no_path_uri.get());
  ScopedCString built_no_path = MakeScopedCString(
      ncuribuild(no_path_uri.get(), nullptr, nullptr, NCURIALL));
  EXPECT_STREQ("https://u%401:p%3a%202@host.test/", built_no_path.get());

  // Hostless file:/// URI build.
  raw = nullptr;
  ASSERT_EQ(NC_NOERR, ncuriparse("file:///tmp/data.nc", &raw));
  ScopedNcUri file_uri(raw);
  ASSERT_NE(nullptr, file_uri.get());
  ScopedCString built_file =
      MakeScopedCString(ncuribuild(file_uri.get(), nullptr, nullptr, NCURIALL));
  EXPECT_STREQ("file:///tmp/data.nc", built_file.get());
}

TEST(NcuriTest, QueryAndFragmentLookupParamsSetKeyAndClone) {
  // Null-guard checks for lookup functions.
  EXPECT_EQ(nullptr, ncurifragmentlookup(nullptr, "k"));
  EXPECT_EQ(nullptr, ncuriquerylookup(nullptr, "k"));

  NCURI* raw = nullptr;
  ASSERT_EQ(NC_NOERR, ncuriparse("http://srv.test/item", &raw));
  ScopedNcUri uri(raw);
  ASSERT_NE(nullptr, uri.get());

  EXPECT_EQ(nullptr, ncurifragmentlookup(uri.get(), nullptr));
  EXPECT_EQ(nullptr, ncuriquerylookup(uri.get(), nullptr));
  EXPECT_EQ(nullptr, ncurifragmentlookup(uri.get(), "missing"));
  EXPECT_EQ(nullptr, ncuriquerylookup(uri.get(), "missing"));
  EXPECT_EQ(nullptr, ncuriqueryparams(uri.get()));
  EXPECT_EQ(nullptr, ncurifragmentparams(uri.get()));

  // Fragment key insertion (initially null list, existing list, overwrite).
  EXPECT_EQ(NC_NOERR, ncurisetfragmentkey(uri.get(), "alpha", "one"));
  EXPECT_EQ(NC_NOERR, ncurisetfragmentkey(uri.get(), "beta", ""));
  EXPECT_EQ(NC_NOERR, ncurisetfragmentkey(uri.get(), "ALPHA", "two"));
  EXPECT_STREQ("two", ncurifragmentlookup(uri.get(), "alpha"));
  EXPECT_STREQ("", ncurifragmentlookup(uri.get(), "BETA"));
  EXPECT_STREQ("alpha=two&beta", uri->fragment);

  auto* frag_list = static_cast<NClist*>(ncurifragmentparams(uri.get()));
  ASSERT_NE(nullptr, frag_list);
  ASSERT_EQ(std::size_t{4}, nclistlength(frag_list));
  EXPECT_STREQ("alpha", static_cast<const char*>(nclistget(frag_list, 0)));
  EXPECT_STREQ("two", static_cast<const char*>(nclistget(frag_list, 1)));
  EXPECT_STREQ("beta", static_cast<const char*>(nclistget(frag_list, 2)));
  EXPECT_STREQ("", static_cast<const char*>(nclistget(frag_list, 3)));

  // Query key insertion: upstream ncurisetquerykey pushes raw key/value
  // pointers when pos < 0 without strdup, so transfer heap-allocated C strings
  // owned by querylist; overwrites (pos >= 0) strdup internally.
  EXPECT_EQ(NC_NOERR,
            ncurisetquerykey(uri.get(), DupCString("q1"), DupCString("v1")));
  EXPECT_EQ(NC_NOERR,
            ncurisetquerykey(uri.get(), DupCString("q2"), DupCString("v2")));
  EXPECT_EQ(NC_NOERR, ncurisetquerykey(uri.get(), "Q1", "v1_updated"));
  EXPECT_STREQ("v1_updated", ncuriquerylookup(uri.get(), "q1"));
  EXPECT_STREQ("v2", ncuriquerylookup(uri.get(), "q2"));
  EXPECT_STREQ("q1=v1_updated&q2=v2", uri->query);

  auto* query_list = static_cast<NClist*>(ncuriqueryparams(uri.get()));
  ASSERT_NE(nullptr, query_list);
  ASSERT_EQ(std::size_t{4}, nclistlength(query_list));
  EXPECT_STREQ("q1", static_cast<const char*>(nclistget(query_list, 0)));
  EXPECT_STREQ("v1_updated",
               static_cast<const char*>(nclistget(query_list, 1)));

  // Deep clone of a full URI lazily rebuilds querylist and fraglist on lookup.
  raw = nullptr;
  ASSERT_EQ(NC_NOERR,
            ncuriparse("https://u:p@host.test:9000/a/b?x=10#y=20", &raw));
  ScopedNcUri orig_full(raw);
  ASSERT_NE(nullptr, orig_full.get());

  ScopedNcUri clone_full(ncuriclone(orig_full.get()));
  ASSERT_NE(nullptr, clone_full.get());
  EXPECT_NE(orig_full.get(), clone_full.get());
  EXPECT_NE(orig_full->uri, clone_full->uri);
  EXPECT_STREQ(orig_full->uri, clone_full->uri);
  EXPECT_STREQ("https", clone_full->protocol);
  EXPECT_STREQ("u", clone_full->user);
  EXPECT_STREQ("p", clone_full->password);
  EXPECT_STREQ("host.test", clone_full->host);
  EXPECT_STREQ("9000", clone_full->port);
  EXPECT_STREQ("/a/b", clone_full->path);
  EXPECT_STREQ("x=10", clone_full->query);
  EXPECT_STREQ("y=20", clone_full->fragment);
  EXPECT_EQ(nullptr, clone_full->querylist);
  EXPECT_EQ(nullptr, clone_full->fraglist);

  // Lookups on cloned URI trigger !haslist && hastext rebuilds.
  EXPECT_STREQ("10", ncuriquerylookup(clone_full.get(), "x"));
  EXPECT_STREQ("20", ncurifragmentlookup(clone_full.get(), "y"));
  EXPECT_NE(nullptr, clone_full->querylist);
  EXPECT_NE(nullptr, clone_full->fraglist);

  // Clone minimal URI with null optional fields.
  ScopedNcUri clone_min(ncuriclone(uri.get()));
  ASSERT_NE(nullptr, clone_min.get());
  EXPECT_EQ(nullptr, clone_min->user);
  EXPECT_EQ(nullptr, clone_min->password);
  EXPECT_EQ(nullptr, clone_min->port);
}

TEST(NcuriTest, PercentEncodingDecodingPartialDecodingAndUnescape) {
  // Null and empty inputs.
  EXPECT_EQ(nullptr, ncuriencodeonly(nullptr, "abc"));
  EXPECT_EQ(nullptr, ncuriencodeuserpwd(nullptr));
  EXPECT_EQ(nullptr, ncuridecode(nullptr));
  EXPECT_EQ(nullptr, ncuridecodepartial(nullptr, "abc"));
  EXPECT_EQ(nullptr, ncuridecodepartial("abc", nullptr));
  EXPECT_EQ(nullptr, ncuriunescape(nullptr));

  ScopedCString empty_enc = MakeScopedCString(ncuriencodeonly("", "abc"));
  EXPECT_STREQ("", empty_enc.get());
  ScopedCString empty_dec = MakeScopedCString(ncuridecode(""));
  EXPECT_STREQ("", empty_dec.get());

  // ncuriencodeonly & ncuriencodeuserpwd.
  ScopedCString custom_enc =
      MakeScopedCString(ncuriencodeonly("az\x0f\x7f <>", "az"));
  EXPECT_STREQ("az%0f%7f%20%3c%3e", custom_enc.get());

  ScopedCString upwd_enc =
      MakeScopedCString(ncuriencodeuserpwd("user name@host:12/a?b#c"));
  EXPECT_STREQ("user%20name%40host%3a12/a?b#c", upwd_enc.get());

  // ncuridecode: lowercase, uppercase, digits, and malformed '%' escapes.
  ScopedCString dec_ok =
      MakeScopedCString(ncuridecode("a%20b%2fC%3a%7E%2F%0F"));
  EXPECT_STREQ("a b/C:~/\x0f", dec_ok.get());

  ScopedCString dec_malformed = MakeScopedCString(ncuridecode("x%y%2z%ZZ%"));
  EXPECT_STREQ("x%y%2z%ZZ%", dec_malformed.get());
  ScopedCString dec_short = MakeScopedCString(ncuridecode("x%2"));
  EXPECT_STREQ("x%2", dec_short.get());

  // ncuridecodepartial: '+' and selective '%XX' decoding.
  ScopedCString pdec_plus_yes =
      MakeScopedCString(ncuridecodepartial("a+b%20c%2Fd%3D%2Z%", "+/="));
  EXPECT_STREQ("a b%20c/d=%2Z%", pdec_plus_yes.get());

  ScopedCString pdec_plus_no =
      MakeScopedCString(ncuridecodepartial("a+b%20c%2Fd%zz%2", " /"));
  EXPECT_STREQ("a+b c/d%zz%2", pdec_plus_no.get());

  // ncuriunescape decodes '+' and printable ASCII while leaving control/0x7f
  // encoded.
  ScopedCString unesc = MakeScopedCString(ncuriunescape("a+b%20%2f%3D%01%7F"));
  EXPECT_STREQ("a b /=%01%7F", unesc.get());
}

TEST(NcuriTest, IsolatedChildCoversAppendKeysAndNullDurip) {
  // Upstream ncuriappendfragmentkey/ncuriappendquerykey call nullfree() instead
  // of nclistfreeall() on the internal list (leaking intermediate entries),
  // ncuriparse(..., nullptr) calls free(duri) without freeing sub-fields, and
  // removedups() calls nclistremove() without freeing removed key/value pairs.
  // Run these paths in an isolated child so parent HeapChecker/LSan stay
  // leak-free.
  RunInExecveChild([]() -> bool {
    if (ncuriparse("http://null-out.test/path", nullptr) != NC_NOERR) {
      return false;
    }

    // Exact case-insensitive (key, value) deduplication via removedups() in
    // ncuriparse(), ncurisetquery(), and ncurisetfragments().
    NCURI* dup_raw = nullptr;
    if (ncuriparse("[a=1][b=x\\]y][flag]https://srv.test/ds#A=1&a=9&c=3",
                   &dup_raw) != NC_NOERR) {
      return false;
    }
    ScopedNcUri dup_uri(dup_raw);
    if (dup_uri.get() == nullptr || dup_uri->fragment == nullptr ||
        std::strcmp(dup_uri->fragment, "a=1&b=x\\]y&flag&a=9&c=3") != 0) {
      return false;
    }
    auto* init_flist = static_cast<NClist*>(ncurifragmentparams(dup_uri.get()));
    if (init_flist == nullptr || nclistlength(init_flist) != std::size_t{10}) {
      return false;
    }
    const char* frag_a = ncurifragmentlookup(dup_uri.get(), "a");
    if (frag_a == nullptr || std::strcmp(frag_a, "1") != 0) {
      return false;
    }

    // ncurisetquery()/ncurisetfragments() deduplicate their internal lists
    // while keeping the caller-supplied query/fragment text intact.
    if (ncurisetquery(dup_uri.get(), "k=v&K=v&m=2") != NC_NOERR ||
        dup_uri->query == nullptr ||
        std::strcmp(dup_uri->query, "k=v&K=v&m=2") != 0) {
      return false;
    }
    auto* qlist = static_cast<NClist*>(ncuriqueryparams(dup_uri.get()));
    if (qlist == nullptr || nclistlength(qlist) != std::size_t{4}) {
      return false;
    }
    const char* qk = ncuriquerylookup(dup_uri.get(), "k");
    const char* qm = ncuriquerylookup(dup_uri.get(), "m");
    if (qk == nullptr || std::strcmp(qk, "v") != 0 || qm == nullptr ||
        std::strcmp(qm, "2") != 0) {
      return false;
    }

    if (ncurisetfragments(dup_uri.get(), "f1=1&F1=1&f2") != NC_NOERR ||
        dup_uri->fragment == nullptr ||
        std::strcmp(dup_uri->fragment, "f1=1&F1=1&f2") != 0) {
      return false;
    }
    auto* flist = static_cast<NClist*>(ncurifragmentparams(dup_uri.get()));
    if (flist == nullptr || nclistlength(flist) != std::size_t{4}) {
      return false;
    }
    const char* f1 = ncurifragmentlookup(dup_uri.get(), "f1");
    const char* f2 = ncurifragmentlookup(dup_uri.get(), "f2");
    if (f1 == nullptr || std::strcmp(f1, "1") != 0 || f2 == nullptr ||
        std::strcmp(f2, "") != 0) {
      return false;
    }
    if (ncurirebuild(dup_uri.get()) != NC_NOERR || dup_uri->uri == nullptr ||
        std::strcmp(dup_uri->uri,
                    "https://srv.test/ds?k=v&K=v&m=2#f1=1&F1=1&f2") != 0) {
      return false;
    }

    NCURI* raw = nullptr;
    if (ncuriparse("http://append.test/path?q1=v1#f1=a", &raw) != NC_NOERR) {
      return false;
    }
    ScopedNcUri uri(raw);

    if (ncuriappendfragmentkey(uri.get(), "f2", "b") != NC_NOERR) {
      return false;
    }
    if (ncuriappendfragmentkey(uri.get(), "f1", "a2") != NC_NOERR) {
      return false;
    }
    if (ncuriappendfragmentkey(uri.get(), "f3", nullptr) != NC_NOERR) {
      return false;
    }
    if (ncuriappendquerykey(uri.get(), "q2", "v2") != NC_NOERR) {
      return false;
    }
    if (ncuriappendquerykey(uri.get(), "q1", "v1b") != NC_NOERR) {
      return false;
    }
    if (ncuriappendquerykey(uri.get(), "q3", nullptr) != NC_NOERR) {
      return false;
    }
    return true;
  });
}

}  // namespace
}  // namespace netcdf
