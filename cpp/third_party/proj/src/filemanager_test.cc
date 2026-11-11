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
// Unit tests for PROJ FileManager, File I/O adapters, resource search paths,
// INI configuration loading, and context file callbacks
// (proj/src/filemanager.cpp). Note: in google3 default non-GCE builds,
// networking is disabled in :proj4 (proj_context_is_network_enabled returns 0;
// :proj4_unbundled marks it weak via google/util/weak_network_enabled.h so this
// test can exercise in-process network-file routing), embedded proj.db uses
// memvfs, and default contexts install google3_proj4_hooks (file_finder +
// fileApi).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/filemanager.cpp

#include "third_party/proj4/proj/src/filemanager.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/google/util/temp_file.h"
#include "third_party/proj4/proj/include/proj/internal/io_internal.hpp"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"
#include "third_party/sqlite/sqlite3.h"

namespace {
constinit std::atomic<bool> g_use_context_network_flag{false};
}  // namespace

extern "C" int proj_context_is_network_enabled(PJ_CONTEXT* ctx) {
  if (!g_use_context_network_flag.load(std::memory_order_relaxed)) {
    return 0;
  }
  PJ_CONTEXT* target = ctx != nullptr ? ctx : pj_get_default_ctx();
  return target->networking.enabled ? 1 : 0;
}

namespace proj {
namespace {

using ::testing::IsEmpty;
using ::testing::NotNull;

// Restores an environment variable's presence and value upon scope exit.
class ScopedEnvVar {
 public:
  explicit ScopedEnvVar(const char* name) : name_(name) {
    if (const char* existing = std::getenv(name_)) {
      original_ = existing;
    }
  }

  ~ScopedEnvVar() {
    if (original_.has_value()) {
      setenv(name_, original_->c_str(), /*replace=*/1);
    } else {
      unsetenv(name_);
    }
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

  void Set(const char* value) const { setenv(name_, value, /*replace=*/1); }
  void Unset() const { unsetenv(name_); }

 private:
  const char* name_;
  std::optional<std::string> original_;
};

struct NoOpFileApiState {
  int dummy_handle = 1;
};

PROJ_FILE_HANDLE* NoOpOpen(PJ_CONTEXT*, const char*, PROJ_OPEN_ACCESS,
                           void* ud) {
  return reinterpret_cast<PROJ_FILE_HANDLE*>(
      &(static_cast<NoOpFileApiState*>(ud)->dummy_handle));
}
size_t NoOpRead(PJ_CONTEXT*, PROJ_FILE_HANDLE*, void*, size_t, void*) {
  return 0;
}
size_t NoOpWrite(PJ_CONTEXT*, PROJ_FILE_HANDLE*, const void*, size_t, void*) {
  return 0;
}
// NOLINTNEXTLINE(runtime/int)
int NoOpSeek(PJ_CONTEXT*, PROJ_FILE_HANDLE*, long long, int, void*) {
  return 1;
}
// NOLINTNEXTLINE(runtime/int)
unsigned long long NoOpTell(PJ_CONTEXT*, PROJ_FILE_HANDLE*, void*) { return 0; }
void NoOpClose(PJ_CONTEXT*, PROJ_FILE_HANDLE*, void*) {}
int NoOpExists(PJ_CONTEXT*, const char*, void*) { return 0; }
int NoOpMkdir(PJ_CONTEXT*, const char*, void*) { return 1; }
int NoOpUnlink(PJ_CONTEXT*, const char*, void*) { return 1; }
int NoOpRename(PJ_CONTEXT*, const char*, const char*, void*) { return 1; }

constexpr PROJ_FILE_API MakeValidNoOpFileApi() {
  return {
      /*version=*/1, &NoOpOpen,   &NoOpRead,   &NoOpWrite,
      &NoOpSeek,     &NoOpTell,   &NoOpClose,  &NoOpExists,
      &NoOpMkdir,    &NoOpUnlink, &NoOpRename,
  };
}

// Custom PROJ_FILE_API that reports a larger file size on SEEK_END+tell than
// read() actually delivers, exercising the short-read abort path in
// pj_load_ini.
struct ShortReadState {
  int handle = 1;
  bool at_end = false;
};

PROJ_FILE_HANDLE* ShortReadOpen(PJ_CONTEXT*, const char* filename,
                                PROJ_OPEN_ACCESS access, void* ud) {
  if (access == PROJ_OPEN_ACCESS_READ_ONLY && filename != nullptr &&
      std::string_view(filename) == "/mem/short_ini/proj.ini") {
    return reinterpret_cast<PROJ_FILE_HANDLE*>(
        &(static_cast<ShortReadState*>(ud)->handle));
  }
  return nullptr;
}

size_t ShortReadRead(PJ_CONTEXT*, PROJ_FILE_HANDLE*, void* buf, size_t size,
                     void*) {
  constexpr std::string_view kPartial = "a=b";
  const size_t n = std::min(size, kPartial.size());
  std::memcpy(buf, kPartial.data(), n);
  return n;
}

// NOLINTNEXTLINE(runtime/int)
int ShortReadSeek(PJ_CONTEXT*, PROJ_FILE_HANDLE*, long long, int whence,
                  void* ud) {
  static_cast<ShortReadState*>(ud)->at_end = (whence == SEEK_END);
  return 1;
}

// NOLINTNEXTLINE(runtime/int)
unsigned long long ShortReadTell(PJ_CONTEXT*, PROJ_FILE_HANDLE*, void* ud) {
  return static_cast<ShortReadState*>(ud)->at_end ? 32ULL : 0ULL;
}

TEST(FileManagerTest, ReadLineHandlesLineEndingsTruncationAndEof) {
  InMemoryFileSystem fs;
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ASSERT_TRUE(fs.Install(ctx.get()));

  // 1. Mixed LF, CRLF, standalone CR, blank line, long buffered line, and
  //    unterminated trailing text.
  fs.AddTextFile("/mem/lines.txt",
                 "alpha\nbeta\r\ngamma\rdelta\n\n0123456789\nrest");
  auto file = NS_PROJ::FileManager::open(ctx.get(), "/mem/lines.txt",
                                         NS_PROJ::FileAccess::READ_ONLY);
  ASSERT_THAT(file, NotNull());
  EXPECT_EQ("/mem/lines.txt", file->name());

  bool max_len_reached = false;
  bool eof_reached = false;

  EXPECT_EQ("alpha", file->read_line(20, max_len_reached, eof_reached));
  EXPECT_FALSE(max_len_reached);
  EXPECT_FALSE(eof_reached);

  EXPECT_EQ("beta", file->read_line(20, max_len_reached, eof_reached));
  EXPECT_FALSE(max_len_reached);
  EXPECT_FALSE(eof_reached);

  EXPECT_EQ("gamma", file->read_line(20, max_len_reached, eof_reached));
  EXPECT_FALSE(max_len_reached);
  EXPECT_FALSE(eof_reached);

  EXPECT_EQ("delta", file->read_line(20, max_len_reached, eof_reached));
  EXPECT_FALSE(max_len_reached);
  EXPECT_FALSE(eof_reached);

  EXPECT_EQ("", file->read_line(20, max_len_reached, eof_reached));
  EXPECT_FALSE(max_len_reached);
  EXPECT_FALSE(eof_reached);

  // Buffer currently holds "0123456789\nre"; passing maxLen=4 triggers the
  // pos > maxLen truncation branch even though '\n' is already buffered.
  EXPECT_EQ("0123", file->read_line(4, max_len_reached, eof_reached));
  EXPECT_TRUE(max_len_reached);
  EXPECT_FALSE(eof_reached);

  EXPECT_EQ("456789", file->read_line(20, max_len_reached, eof_reached));
  EXPECT_FALSE(max_len_reached);
  EXPECT_FALSE(eof_reached);

  // Unterminated final line returns text with eof_reached == false, followed by
  // empty string with eof_reached == true (and clamps maxLen > 1 MiB).
  EXPECT_EQ("rest", file->read_line(20, max_len_reached, eof_reached));
  EXPECT_FALSE(max_len_reached);
  EXPECT_FALSE(eof_reached);

  EXPECT_EQ("",
            file->read_line(2u * 1024u * 1024u, max_len_reached, eof_reached));
  EXPECT_FALSE(max_len_reached);
  EXPECT_TRUE(eof_reached);

  // 2. Line without any newline hits maxLen <= prevSize before EOF.
  fs.AddTextFile("/mem/no_eol.txt", "abcdefghij");
  auto no_eol = NS_PROJ::FileManager::open(ctx.get(), "/mem/no_eol.txt",
                                           NS_PROJ::FileAccess::READ_ONLY);
  ASSERT_THAT(no_eol, NotNull());
  EXPECT_EQ("abcd", no_eol->read_line(4, max_len_reached, eof_reached));
  EXPECT_TRUE(max_len_reached);
  EXPECT_FALSE(eof_reached);
  EXPECT_EQ("efgh", no_eol->read_line(4, max_len_reached, eof_reached));
  EXPECT_TRUE(max_len_reached);
  EXPECT_FALSE(eof_reached);
  EXPECT_EQ("ij", no_eol->read_line(4, max_len_reached, eof_reached));
  EXPECT_FALSE(max_len_reached);
  EXPECT_FALSE(eof_reached);
  EXPECT_EQ("", no_eol->read_line(4, max_len_reached, eof_reached));
  EXPECT_FALSE(max_len_reached);
  EXPECT_TRUE(eof_reached);

  // 3. Empty file reaches EOF immediately.
  fs.AddTextFile("/mem/empty.txt", "");
  auto empty = NS_PROJ::FileManager::open(ctx.get(), "/mem/empty.txt",
                                          NS_PROJ::FileAccess::READ_ONLY);
  ASSERT_THAT(empty, NotNull());
  EXPECT_EQ("", empty->read_line(16, max_len_reached, eof_reached));
  EXPECT_FALSE(max_len_reached);
  EXPECT_TRUE(eof_reached);
}

TEST(FileManagerTest, FileApiAdapterSupportsAllAccessModesAndOperations) {
  InMemoryFileSystem fs;
  ScopedContext ctx1 = MakeContext();
  ScopedContext ctx2 = MakeContext();
  ASSERT_NE(nullptr, ctx1.get());
  ASSERT_NE(nullptr, ctx2.get());

  ASSERT_TRUE(fs.Install(ctx1.get()));
  ASSERT_TRUE(fs.Install(ctx2.get()));

  // 1. Opening a missing file read-only returns nullptr.
  EXPECT_EQ(nullptr, NS_PROJ::FileManager::open(ctx1.get(), "/mem/missing.bin",
                                                NS_PROJ::FileAccess::READ_ONLY)
                         .get());

  // 2. CREATE mode writes, seeks (SET/CUR/invalid), tells, and reassigns ctx.
  {
    auto file = NS_PROJ::FileManager::open(ctx1.get(), "/mem/data.bin",
                                           NS_PROJ::FileAccess::CREATE);
    ASSERT_THAT(file, NotNull());
    EXPECT_FALSE(file->hasChanged());
    EXPECT_EQ(6u, file->write("abcdef", 6));
    EXPECT_EQ(6u, file->tell());
    EXPECT_TRUE(file->seek(2, SEEK_SET));
    EXPECT_EQ(2u, file->tell());
    EXPECT_TRUE(file->seek(2, SEEK_CUR));
    EXPECT_EQ(4u, file->tell());
    EXPECT_FALSE(file->seek(static_cast<unsigned long long>(-5), SEEK_SET));
    file->reassign_context(ctx2.get());
  }

  // 3. READ_UPDATE mode updates an existing file in place.
  {
    auto file = NS_PROJ::FileManager::open(ctx1.get(), "/mem/data.bin",
                                           NS_PROJ::FileAccess::READ_UPDATE);
    ASSERT_THAT(file, NotNull());
    EXPECT_TRUE(file->seek(2, SEEK_SET));
    EXPECT_EQ(2u, file->write("XY", 2));
    EXPECT_TRUE(file->seek(0, SEEK_END));
    EXPECT_EQ(6u, file->tell());
  }

  // 4. READ_ONLY mode reads updated content and rejects writes.
  {
    auto file = NS_PROJ::FileManager::open(ctx1.get(), "/mem/data.bin",
                                           NS_PROJ::FileAccess::READ_ONLY);
    ASSERT_THAT(file, NotNull());
    char buf[7] = {};
    EXPECT_EQ(6u, file->read(buf, 6));
    EXPECT_STREQ("abXYef", buf);
    EXPECT_EQ(0u, file->write("z", 1));
  }

  // 5. Directory and file manipulation helpers delegate to PROJ_FILE_API.
  EXPECT_TRUE(NS_PROJ::FileManager::exists(ctx1.get(), "/mem/data.bin"));
  EXPECT_FALSE(NS_PROJ::FileManager::exists(ctx1.get(), "/mem/absent.bin"));
  EXPECT_TRUE(NS_PROJ::FileManager::mkdir(ctx1.get(), "/mem/newdir"));
  EXPECT_TRUE(NS_PROJ::FileManager::exists(ctx1.get(), "/mem/newdir"));
  EXPECT_TRUE(
      NS_PROJ::FileManager::rename(ctx1.get(), "/mem/data.bin", "/mem/b.bin"));
  EXPECT_FALSE(NS_PROJ::FileManager::exists(ctx1.get(), "/mem/data.bin"));
  EXPECT_TRUE(NS_PROJ::FileManager::exists(ctx1.get(), "/mem/b.bin"));
  EXPECT_FALSE(
      NS_PROJ::FileManager::rename(ctx1.get(), "/mem/data.bin", "/mem/c.bin"));
  EXPECT_TRUE(NS_PROJ::FileManager::unlink(ctx1.get(), "/mem/b.bin"));
  EXPECT_FALSE(NS_PROJ::FileManager::unlink(ctx1.get(), "/mem/b.bin"));
}

TEST(FileManagerTest, StdioFallbackExercisesPosixFileAndDirOperations) {
  ScopedContext ctx1 = MakeContext();
  ScopedContext ctx2 = MakeContext();
  ASSERT_NE(nullptr, ctx1.get());
  ASSERT_NE(nullptr, ctx2.get());
  // Clear google3's installed fileApi on these local contexts so FileManager
  // routes through FileStdio and POSIX stat/mkdir/unlink/rename.
  ctx1->fileApi = {};
  ctx2->fileApi = {};

  TempDir dir("filemanager_stdio");
  const std::filesystem::path file_a = dir.child("alpha.bin");
  const std::filesystem::path file_b = dir.child("beta.bin");
  const std::filesystem::path subdir = dir.child("sub/nested");

  // 1. READ_ONLY on missing file returns nullptr.
  EXPECT_EQ(nullptr, NS_PROJ::FileManager::open(ctx1.get(), file_a.c_str(),
                                                NS_PROJ::FileAccess::READ_ONLY)
                         .get());

  // 2. CREATE, write, seek (SET/CUR/END/negative failure), tell, read, and
  //    reassign context cleanly.
  {
    auto file = NS_PROJ::FileManager::open(ctx1.get(), file_a.c_str(),
                                           NS_PROJ::FileAccess::CREATE);
    ASSERT_THAT(file, NotNull());
    EXPECT_FALSE(file->hasChanged());
    EXPECT_EQ(5u, file->write("hello", 5));
    EXPECT_EQ(5u, file->tell());
    EXPECT_TRUE(file->seek(0, SEEK_SET));
    EXPECT_EQ(0u, file->tell());
    EXPECT_TRUE(file->seek(2, SEEK_CUR));
    EXPECT_EQ(2u, file->tell());
    EXPECT_TRUE(file->seek(0, SEEK_END));
    EXPECT_EQ(5u, file->tell());
    EXPECT_TRUE(file->seek(0, SEEK_SET));
    char buf[6] = {};
    EXPECT_EQ(5u, file->read(buf, 5));
    EXPECT_STREQ("hello", buf);
    EXPECT_FALSE(file->seek(static_cast<unsigned long long>(-10), SEEK_SET));
    errno = 0;
    file->reassign_context(ctx2.get());
  }

  // 3. READ_UPDATE and READ_ONLY (including read_line) on the created file.
  {
    auto file = NS_PROJ::FileManager::open(ctx1.get(), file_a.c_str(),
                                           NS_PROJ::FileAccess::READ_UPDATE);
    ASSERT_THAT(file, NotNull());
    EXPECT_TRUE(file->seek(1, SEEK_SET));
    EXPECT_EQ(1u, file->write("a", 1));
  }
  {
    auto file = NS_PROJ::FileManager::open(ctx1.get(), file_a.c_str(),
                                           NS_PROJ::FileAccess::READ_ONLY);
    ASSERT_THAT(file, NotNull());
    bool max_len_reached = false;
    bool eof_reached = false;
    EXPECT_EQ("hallo", file->read_line(32, max_len_reached, eof_reached));
    EXPECT_FALSE(max_len_reached);
    EXPECT_FALSE(eof_reached);
  }

  // 4. POSIX exists / mkdir / rename / unlink + recursive directory creation.
  EXPECT_TRUE(NS_PROJ::FileManager::exists(ctx1.get(), file_a.c_str()));
  EXPECT_FALSE(NS_PROJ::FileManager::exists(ctx1.get(), file_b.c_str()));
  EXPECT_TRUE(
      NS_PROJ::FileManager::rename(ctx1.get(), file_a.c_str(), file_b.c_str()));
  EXPECT_FALSE(NS_PROJ::FileManager::exists(ctx1.get(), file_a.c_str()));
  EXPECT_TRUE(NS_PROJ::FileManager::exists(ctx1.get(), file_b.c_str()));
  EXPECT_FALSE(NS_PROJ::FileManager::rename(
      ctx1.get(), dir.child("nonexistent_src.bin").c_str(), file_b.c_str()));
  EXPECT_TRUE(NS_PROJ::FileManager::unlink(ctx1.get(), file_b.c_str()));
  EXPECT_FALSE(NS_PROJ::FileManager::unlink(ctx1.get(), file_b.c_str()));

  const std::filesystem::path single_dir = dir.child("single");
  EXPECT_TRUE(NS_PROJ::FileManager::mkdir(ctx1.get(), single_dir.c_str()));
  EXPECT_FALSE(NS_PROJ::FileManager::mkdir(ctx1.get(), single_dir.c_str()));

  proj_context_set_user_writable_directory(ctx1.get(), subdir.c_str(), TRUE);
  EXPECT_TRUE(NS_PROJ::FileManager::exists(ctx1.get(), subdir.c_str()));
}

TEST(FileManagerTest, SetFileApiValidationAndSqliteVfsSetter) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Null struct or wrong version is rejected.
  EXPECT_EQ(0, proj_context_set_fileapi(ctx.get(), nullptr, nullptr));

  PROJ_FILE_API bad_version = MakeValidNoOpFileApi();
  bad_version.version = 2;
  EXPECT_EQ(0, proj_context_set_fileapi(ctx.get(), &bad_version, nullptr));

  // 2. Missing any of the 10 callbacks is rejected.
  const std::vector<void (*)(PROJ_FILE_API&)> clear_cbks = {
      [](PROJ_FILE_API& api) { api.open_cbk = nullptr; },
      [](PROJ_FILE_API& api) { api.close_cbk = nullptr; },
      [](PROJ_FILE_API& api) { api.read_cbk = nullptr; },
      [](PROJ_FILE_API& api) { api.write_cbk = nullptr; },
      [](PROJ_FILE_API& api) { api.seek_cbk = nullptr; },
      [](PROJ_FILE_API& api) { api.tell_cbk = nullptr; },
      [](PROJ_FILE_API& api) { api.exists_cbk = nullptr; },
      [](PROJ_FILE_API& api) { api.mkdir_cbk = nullptr; },
      [](PROJ_FILE_API& api) { api.unlink_cbk = nullptr; },
      [](PROJ_FILE_API& api) { api.rename_cbk = nullptr; },
  };
  for (size_t i = 0; i < clear_cbks.size(); ++i) {
    PROJ_FILE_API api = MakeValidNoOpFileApi();
    clear_cbks[i](api);
    EXPECT_EQ(0, proj_context_set_fileapi(ctx.get(), &api, nullptr))
        << "callback index " << i;
  }

  // 3. Valid PROJ_FILE_API installs callbacks and user_data.
  NoOpFileApiState state;
  const PROJ_FILE_API valid = MakeValidNoOpFileApi();
  EXPECT_EQ(1, proj_context_set_fileapi(ctx.get(), &valid, &state));
  EXPECT_EQ(&state, ctx->fileApi.user_data);

  // 4. SQLite3 VFS name sets and clears cleanly.
  proj_context_set_sqlite3_vfs_name(ctx.get(), "memvfs_custom");
  EXPECT_EQ("memvfs_custom", ctx->custom_sqlite3_vfs_name);
  proj_context_set_sqlite3_vfs_name(ctx.get(), nullptr);
  EXPECT_THAT(ctx->custom_sqlite3_vfs_name, IsEmpty());
}

TEST(FileManagerTest, NetworkOpenIsBlockedWhenNetworkIsDisabled) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  EXPECT_EQ(0, proj_context_is_network_enabled(ctx.get()));

  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
  EXPECT_EQ(nullptr,
            NS_PROJ::FileManager::open(ctx.get(), "http://example.test/g.tif",
                                       NS_PROJ::FileAccess::READ_ONLY)
                .get());
  EXPECT_TRUE(capture.ContainsMessage("not authorized"));

  capture.Clear();
  EXPECT_EQ(nullptr,
            NS_PROJ::FileManager::open(ctx.get(), "https://example.test/g.tif",
                                       NS_PROJ::FileAccess::READ_ONLY)
                .get());
  EXPECT_TRUE(capture.ContainsMessage("not authorized"));
}

TEST(FileManagerTest, UserWritableDirectoryEnvFallbacksAndRecursiveCreate) {
  ScopedEnvVar env_uwd("PROJ_USER_WRITABLE_DIRECTORY");
  ScopedEnvVar env_xdg("XDG_DATA_HOME");
  ScopedEnvVar env_home("HOME");

  // 1. Explicit PROJ_USER_WRITABLE_DIRECTORY takes precedence.
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    env_uwd.Set("/custom/uwd");
    env_xdg.Set("/custom/xdg");
    env_home.Set("/custom/home");
    EXPECT_STREQ("/custom/uwd",
                 proj_context_get_user_writable_directory(ctx.get(), FALSE));
  }

  // 2. XDG_DATA_HOME fallback appends "/proj".
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    env_uwd.Unset();
    env_xdg.Set("/custom/xdg");
    EXPECT_STREQ("/custom/xdg/proj",
                 proj_context_get_user_writable_directory(ctx.get(), FALSE));
  }

  // 3. Writable HOME fallback appends "/.local/share/proj".
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    TempDir home_dir("writable_home");
    env_uwd.Set("");
    env_xdg.Unset();
    env_home.Set(home_dir.c_str());
    const std::string expected = home_dir.string() + "/.local/share/proj";
    EXPECT_STREQ(expected.c_str(),
                 proj_context_get_user_writable_directory(ctx.get(), FALSE));
  }

  // 4. Unset HOME and non-writable HOME both fall back to "/tmp/proj" without
  //    creating anything on disk (create = FALSE).
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    env_uwd.Unset();
    env_xdg.Unset();
    env_home.Unset();
    EXPECT_STREQ("/tmp/proj",
                 proj_context_get_user_writable_directory(ctx.get(), FALSE));
  }
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    env_uwd.Unset();
    env_xdg.Unset();
    env_home.Set("/nonexistent_home_dir_for_proj_test");
    EXPECT_STREQ("/tmp/proj",
                 proj_context_get_user_writable_directory(ctx.get(), FALSE));
  }

  // 5. Resetting path to nullptr recomputes default; create=TRUE exercises
  //    CreateDirectoryRecursively stops at '/' (pos==0), bare name (npos),
  //    and already-existing parent stops.
  {
    InMemoryFileSystem fs;
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ASSERT_TRUE(fs.Install(ctx.get()));

    // Single-component paths stop before mkdir because pos==0 or pos==npos.
    proj_context_set_user_writable_directory(ctx.get(), "/only_top", TRUE);
    EXPECT_FALSE(fs.ContainsDir("/only_top"));
    proj_context_set_user_writable_directory(ctx.get(), "rel_only", TRUE);
    EXPECT_FALSE(fs.ContainsDir("rel_only"));

    // Existing top-level parent allows recursive creation of nested subdirs.
    ASSERT_TRUE(NS_PROJ::FileManager::mkdir(ctx.get(), "/mem_top"));
    proj_context_set_user_writable_directory(ctx.get(), "/mem_top/a/b", TRUE);
    EXPECT_TRUE(fs.ContainsDir("/mem_top/a"));
    EXPECT_TRUE(fs.ContainsDir("/mem_top/a/b"));
    // Second call hits early return when target directory already exists.
    proj_context_set_user_writable_directory(ctx.get(), "/mem_top/a/b", TRUE);

    ASSERT_TRUE(NS_PROJ::FileManager::mkdir(ctx.get(), "rel_top"));
    proj_context_set_user_writable_directory(ctx.get(), "rel_top/sub", TRUE);
    EXPECT_TRUE(fs.ContainsDir("rel_top/sub"));

    env_uwd.Set("/env/recomputed");
    proj_context_set_user_writable_directory(ctx.get(), nullptr, FALSE);
    EXPECT_STREQ("/env/recomputed",
                 proj_context_get_user_writable_directory(ctx.get(), FALSE));
  }
}

TEST(FileManagerTest, ProjDataAndProjLibEnvVarAndDeprecationWarning) {
  ScopedEnvVar env_data("PROJ_DATA");
  ScopedEnvVar env_lib("PROJ_LIB");

  // 1. Neither set returns empty string.
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    env_data.Unset();
    env_lib.Unset();
    EXPECT_EQ("", NS_PROJ::FileManager::getProjDataEnvVar(ctx.get()));
    pj_stderr_proj_lib_deprecation_warning();
  }

  // 2. PROJ_DATA is cached on the context after first read.
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    env_data.Set("/mem/proj_data_one");
    env_lib.Set("/mem/legacy_ignored");
    EXPECT_EQ("/mem/proj_data_one",
              NS_PROJ::FileManager::getProjDataEnvVar(ctx.get()));
    env_data.Set("/mem/proj_data_two");
    EXPECT_EQ("/mem/proj_data_one",
              NS_PROJ::FileManager::getProjDataEnvVar(ctx.get()));
    pj_stderr_proj_lib_deprecation_warning();
  }

  // 3. PROJ_LIB fallback logs a deprecation message when PROJ_DATA is unset.
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    env_data.Unset();
    env_lib.Set("/mem/legacy_proj_lib");
    ScopedLogCapture capture(ctx.get(), PJ_LOG_DEBUG);
    EXPECT_EQ("/mem/legacy_proj_lib",
              NS_PROJ::FileManager::getProjDataEnvVar(ctx.get()));
    EXPECT_TRUE(capture.ContainsMessage("PROJ_LIB"));
    pj_stderr_proj_lib_deprecation_warning();
  }
}

const char* CustomFinder(PJ_CONTEXT*, const char* name, void* user_data) {
  if (name != nullptr && std::string_view(name) == "via_finder.txt") {
    return static_cast<const char*>(user_data);
  }
  return nullptr;
}

TEST(FileManagerTest, OpenResourceFileResolvesAllSearchOrderBranches) {
  ScopedEnvVar env_skip("PROJ_SKIP_READ_USER_WRITABLE_DIRECTORY");
  ScopedEnvVar env_data("PROJ_DATA");
  ScopedEnvVar env_lib("PROJ_LIB");
  ScopedEnvVar env_home("HOME");

  // 1. Default google3 context hooks open embedded gridshift files directly.
  {
    ScopedContext default_hooks_ctx = MakeContext();
    ASSERT_NE(nullptr, default_hooks_ctx.get());
    auto embedded = NS_PROJ::FileManager::open_resource_file(
        default_hooks_ctx.get(), "null");
    EXPECT_THAT(embedded, NotNull());
  }

  InMemoryFileSystem fs;
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ASSERT_TRUE(fs.Install(ctx.get()));
  proj_context_set_file_finder(ctx.get(), nullptr, nullptr);

  // 2. ~/path with HOME set vs HOME unset, and non-tilde "~..." names.
  fs.AddTextFile("/mem_home/~/home_grid.txt", "home");
  env_home.Set("/mem_home");
  char resolved[256] = {};
  {
    auto f = NS_PROJ::FileManager::open_resource_file(
        ctx.get(), "~/home_grid.txt", resolved, sizeof(resolved));
    ASSERT_THAT(f, NotNull());
    EXPECT_STREQ("/mem_home/~/home_grid.txt", resolved);
  }
  env_home.Unset();
  EXPECT_EQ(nullptr,
            NS_PROJ::FileManager::open_resource_file(
                ctx.get(), "~/home_grid.txt", resolved, sizeof(resolved))
                .get());

  // 3. Absolute, relative (./, ../), and Windows-style drive paths bypass
  //    search paths, and small or zero out_full_filename buffers handle
  //    cleanly.
  fs.AddTextFile("/mem/abs.txt", "abs");
  fs.AddTextFile("./rel.txt", "rel");
  fs.AddTextFile("../up.txt", "up");
  fs.AddTextFile("D:/win.txt", "win");
  char tiny_buf[6] = {};
  EXPECT_THAT(NS_PROJ::FileManager::open_resource_file(
                  ctx.get(), "/mem/abs.txt", tiny_buf, sizeof(tiny_buf)),
              NotNull());
  EXPECT_STREQ("/mem/", tiny_buf);
  EXPECT_THAT(NS_PROJ::FileManager::open_resource_file(
                  ctx.get(), "/mem/abs.txt", resolved, 0),
              NotNull());
  EXPECT_THAT(NS_PROJ::FileManager::open_resource_file(ctx.get(), "./rel.txt"),
              NotNull());
  EXPECT_THAT(NS_PROJ::FileManager::open_resource_file(ctx.get(), "../up.txt"),
              NotNull());
  EXPECT_THAT(NS_PROJ::FileManager::open_resource_file(ctx.get(), "D:/win.txt"),
              NotNull());

  // 4. http:// and https:// URLs fail cleanly with network disabled.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                           ctx.get(), "http://cdn.example.test/grid.tif")
                           .get());
    EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                           ctx.get(), "https://cdn.example.test/grid.tif")
                           .get());
  }

  // 5. Custom file_finder resolves before search_paths, or returns nullptr to
  //    fall through.
  fs.AddTextFile("/mem/finder_target.txt", "finder");
  char finder_path[] = "/mem/finder_target.txt";
  proj_context_set_file_finder(ctx.get(), &CustomFinder, finder_path);
  EXPECT_THAT(NS_PROJ::FileManager::open_resource_file(
                  ctx.get(), "via_finder.txt", resolved, sizeof(resolved)),
              NotNull());
  EXPECT_STREQ("/mem/finder_target.txt", resolved);

  // 6. search_paths iterates multiple directories until a match is found.
  fs.AddTextFile("/mem/sp2/in_sp2.txt", "sp2");
  const char* search_dirs[] = {"/mem/sp1", "/mem/sp2"};
  proj_context_set_search_paths(ctx.get(), 2, search_dirs);
  EXPECT_THAT(NS_PROJ::FileManager::open_resource_file(
                  ctx.get(), "in_sp2.txt", resolved, sizeof(resolved)),
              NotNull());
  EXPECT_STREQ("/mem/sp2/in_sp2.txt", resolved);
  EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                         ctx.get(), "missing_in_sp.txt")
                         .get());

  // 7. Clear file_finder & search_paths to test user_writable_directory,
  //    quoted colon-separated PROJ_DATA, and bare-filename fallback.
  proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
  proj_context_set_search_paths(ctx.get(), 0, nullptr);
  env_skip.Unset();
  proj_context_set_user_writable_directory(ctx.get(), "/mem/uwd", FALSE);
  fs.AddTextFile("/mem/uwd/in_uwd.txt", "uwd");
  EXPECT_THAT(NS_PROJ::FileManager::open_resource_file(
                  ctx.get(), "in_uwd.txt", resolved, sizeof(resolved)),
              NotNull());
  EXPECT_STREQ("/mem/uwd/in_uwd.txt", resolved);

  env_skip.Set("1");
  env_data.Set("/mem/pd1:\"/mem/pd2\"");
  env_lib.Unset();
  fs.AddTextFile("/mem/pd2/in_pd2.txt", "pd2");
  EXPECT_THAT(NS_PROJ::FileManager::open_resource_file(
                  ctx.get(), "in_pd2.txt", resolved, sizeof(resolved)),
              NotNull());
  EXPECT_STREQ("/mem/pd2/in_pd2.txt", resolved);
  // Missing in PROJ_DATA falls through cleanly.
  EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                         ctx.get(), "not_in_proj_data.txt")
                         .get());

  // 8. Bare-filename fallback when PROJ_DATA is unset and skip-uwd is active.
  ScopedContext bare_ctx = MakeContext();
  ASSERT_NE(nullptr, bare_ctx.get());
  ASSERT_TRUE(fs.Install(bare_ctx.get()));
  proj_context_set_file_finder(bare_ctx.get(), nullptr, nullptr);
  env_data.Unset();
  env_lib.Unset();
  fs.AddTextFile("bare_name.txt", "bare");
  EXPECT_THAT(NS_PROJ::FileManager::open_resource_file(
                  bare_ctx.get(), "bare_name.txt", resolved, sizeof(resolved)),
              NotNull());
  EXPECT_STREQ("bare_name.txt", resolved);

  // 9. Stdio failure propagates OS errno into ctx->last_errno.
  ScopedContext err_ctx = MakeContext();
  ASSERT_NE(nullptr, err_ctx.get());
  err_ctx->fileApi = {};
  proj_context_set_file_finder(err_ctx.get(), nullptr, nullptr);
  TempDir empty_dir("err_stdio");
  const char* disk_dir = empty_dir.c_str();
  proj_context_set_search_paths(err_ctx.get(), 1, &disk_dir);
  EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                         err_ctx.get(), "definitely_missing_on_disk.dat")
                         .get());
  EXPECT_NE(0, proj_context_errno(err_ctx.get()));
}

TEST(FileManagerTest, OpenResourceFileGridNameMappingAndDbExceptionPaths) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  ScopedEnvVar env_skip("PROJ_SKIP_READ_USER_WRITABLE_DIRECTORY");
  ScopedEnvVar env_data("PROJ_DATA");
  ScopedEnvVar env_lib("PROJ_LIB");
  env_skip.Set("1");
  env_data.Unset();
  env_lib.Unset();

  char resolved[256] = {};

  // 1. Old grid name ("conus") falls back to DB's new name
  //    ("us_noaa_conus.tif") and clears prior errno on success.
  {
    InMemoryFileSystem fs;
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ASSERT_TRUE(fs.Install(ctx.get()));
    proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
    const char* dir = "/mem/grids";
    proj_context_set_search_paths(ctx.get(), 1, &dir);
    fs.AddTextFile("/mem/grids/us_noaa_conus.tif", "fake_tif");

    proj_context_errno_set(ctx.get(), ENOENT);
    auto f = NS_PROJ::FileManager::open_resource_file(
        ctx.get(), "conus", resolved, sizeof(resolved));
    ASSERT_THAT(f, NotNull());
    EXPECT_STREQ("/mem/grids/us_noaa_conus.tif", resolved);
    EXPECT_EQ(0, proj_context_errno(ctx.get()));

    // Old name whose mapped .tif is also absent leaves file null.
    EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                           ctx.get(), "alaska", resolved, sizeof(resolved))
                           .get());
    // "proj.db", "~...", ".rel", drive-colon non-slash, and unknown names skip.
    EXPECT_EQ(
        nullptr,
        NS_PROJ::FileManager::open_resource_file(ctx.get(), "proj.db").get());
    EXPECT_EQ(nullptr,
              NS_PROJ::FileManager::open_resource_file(ctx.get(), "~not_slash")
                  .get());
    EXPECT_EQ(
        nullptr,
        NS_PROJ::FileManager::open_resource_file(ctx.get(), ".not_rel").get());
    EXPECT_EQ(nullptr,
              NS_PROJ::FileManager::open_resource_file(ctx.get(), "C:not_slash")
                  .get());
    EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                           ctx.get(), "no_such_grid_name")
                           .get());
  }

  // 2. New .tif grid name falls back to DB's old name ("conus").
  {
    InMemoryFileSystem fs;
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ASSERT_TRUE(fs.Install(ctx.get()));
    proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
    const char* dir = "/mem/grids";
    proj_context_set_search_paths(ctx.get(), 1, &dir);
    fs.AddTextFile("/mem/grids/conus", "fake_conus");

    proj_context_errno_set(ctx.get(), ENOENT);
    auto f = NS_PROJ::FileManager::open_resource_file(
        ctx.get(), "us_noaa_conus.tif", resolved, sizeof(resolved));
    ASSERT_THAT(f, NotNull());
    EXPECT_STREQ("/mem/grids/conus", resolved);
    EXPECT_EQ(0, proj_context_errno(ctx.get()));

    EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                           ctx.get(), "us_noaa_alaska.tif")
                           .get());
    EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                           ctx.get(), "unknown_grid_xyz.tif")
                           .get());
  }

  // 3. DatabaseContext creation failure inside getDBcontext is caught cleanly.
  {
    InMemoryFileSystem fs;
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ASSERT_TRUE(fs.Install(ctx.get()));
    proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
    proj_context_delete_cpp_context(ctx->cpp_context);
    ctx->cpp_context = new projCppContext(ctx.get(), "/nonexistent/missing.db");

    ScopedLogCapture capture(ctx.get(), PJ_LOG_DEBUG);
    EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                           ctx.get(), "missing_grid_name")
                           .get());
    EXPECT_TRUE(capture.ContainsMessage("missing.db"));
  }

  // 4. Valid SQLite DB lacking the grid_alternatives table exercises both SQL
  //    catch blocks in open_resource_file (non-.tif and .tif).
  {
    InMemoryFileSystem fs;
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    const char* major_ptr = proj_context_get_database_metadata(
        ctx.get(), "DATABASE.LAYOUT.VERSION.MAJOR");
    ASSERT_NE(nullptr, major_ptr);
    const std::string major(major_ptr);
    const char* minor_ptr = proj_context_get_database_metadata(
        ctx.get(), "DATABASE.LAYOUT.VERSION.MINOR");
    ASSERT_NE(nullptr, minor_ptr);
    const std::string minor(minor_ptr);

    TempFile db_file("minimal_no_grid_alt.db");
    sqlite3* raw_db = nullptr;
    const int open_rc =
        sqlite3_open_v2(db_file.c_str(), &raw_db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    auto close_db = [](sqlite3* db) { sqlite3_close(db); };
    std::unique_ptr<sqlite3, decltype(close_db)> db(raw_db, close_db);
    ASSERT_EQ(SQLITE_OK, open_rc);

    ASSERT_EQ(
        SQLITE_OK,
        sqlite3_exec(db.get(), "CREATE TABLE metadata (key TEXT, value TEXT);",
                     nullptr, nullptr, nullptr));
    const auto insert_meta = [&](const char* k, const std::string& v) {
      sqlite3_stmt* stmt = nullptr;
      ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                               db.get(), "INSERT INTO metadata VALUES (?, ?);",
                               -1, &stmt, nullptr));
      absl::Cleanup finalize = [stmt] { sqlite3_finalize(stmt); };
      ASSERT_EQ(SQLITE_OK, sqlite3_bind_text(stmt, 1, k, -1, nullptr));
      ASSERT_EQ(SQLITE_OK, sqlite3_bind_text(stmt, 2, v.c_str(), -1, nullptr));
      ASSERT_EQ(SQLITE_DONE, sqlite3_step(stmt));
    };
    insert_meta("DATABASE.LAYOUT.VERSION.MAJOR", major);
    insert_meta("DATABASE.LAYOUT.VERSION.MINOR", minor);
    db.reset();

    ASSERT_EQ(1, proj_context_set_database_path(ctx.get(), db_file.c_str(),
                                                nullptr, nullptr));
    ASSERT_TRUE(fs.Install(ctx.get()));
    proj_context_set_file_finder(ctx.get(), nullptr, nullptr);

    ScopedLogCapture capture(ctx.get(), PJ_LOG_DEBUG);
    EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                           ctx.get(), "missing_no_tif")
                           .get());
    EXPECT_TRUE(capture.ContainsMessage("grid_alternatives"));

    capture.Clear();
    EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                           ctx.get(), "missing_with.tif")
                           .get());
    EXPECT_TRUE(capture.ContainsMessage("grid_alternatives"));
  }
}

TEST(FileManagerTest, DefaultSearchPathsAndRelativeShareProjBranches) {
  ScopedEnvVar env_skip("PROJ_SKIP_READ_USER_WRITABLE_DIRECTORY");
  ScopedEnvVar env_data("PROJ_DATA");
  ScopedEnvVar env_lib("PROJ_LIB");

  // 1. With skip unset and PROJ_DATA set, both user writable dir and PROJ_DATA
  //    are included.
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    env_skip.Unset();
    env_data.Set("/mem/default_pd");
    env_lib.Unset();
    proj_context_set_user_writable_directory(ctx.get(), "/mem/default_uwd",
                                             FALSE);

    const std::vector<std::string> paths =
        pj_get_default_searchpaths(ctx.get());
    EXPECT_NE(paths.end(), std::find(paths.begin(), paths.end(),
                                     std::string("/mem/default_uwd")));
    EXPECT_NE(paths.end(), std::find(paths.begin(), paths.end(),
                                     std::string("/mem/default_pd")));
  }

  // 2. With skip set and PROJ_DATA unset, user writable dir and env path are
  //    omitted.
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    env_skip.Set("1");
    env_data.Unset();
    env_lib.Unset();
    proj_context_set_user_writable_directory(ctx.get(), "/mem/skipped_uwd",
                                             FALSE);

    const std::vector<std::string> paths =
        pj_get_default_searchpaths(ctx.get());
    EXPECT_EQ(paths.end(), std::find(paths.begin(), paths.end(),
                                     std::string("/mem/skipped_uwd")));
  }

  // 3. Relative share/proj lookup runs cleanly (empty in test sandbox).
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    EXPECT_EQ(pj_get_relative_share_proj(ctx.get()),
              pj_get_relative_share_proj(nullptr));
  }
}

TEST(FileManagerTest, PjFindFileCachesFoundAndMissingResults) {
  ScopedEnvVar env_skip("PROJ_SKIP_READ_USER_WRITABLE_DIRECTORY");
  ScopedEnvVar env_data("PROJ_DATA");
  ScopedEnvVar env_lib("PROJ_LIB");
  env_skip.Set("1");
  env_data.Unset();
  env_lib.Unset();

  InMemoryFileSystem fs;
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ASSERT_TRUE(fs.Install(ctx.get()));
  proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
  const char* dir = "/mem/find_dir";
  proj_context_set_search_paths(ctx.get(), 1, &dir);
  fs.AddTextFile("/mem/find_dir/found.txt", "ok");

  char out[256] = {};
  // 1. First lookup hits open_resource_file and caches resolved path.
  EXPECT_EQ(1, pj_find_file(ctx.get(), "found.txt", out, sizeof(out), true));
  EXPECT_STREQ("/mem/find_dir/found.txt", out);

  // 2. Remove file from fs; cached lookups still return 1 (and truncate).
  EXPECT_TRUE(
      NS_PROJ::FileManager::unlink(ctx.get(), "/mem/find_dir/found.txt"));
  out[0] = '\0';
  EXPECT_EQ(1, pj_find_file(ctx.get(), "found.txt", out, sizeof(out), false));
  EXPECT_STREQ("/mem/find_dir/found.txt", out);
  char small_out[6] = {};
  EXPECT_EQ(1, pj_find_file(ctx.get(), "found.txt", small_out,
                            sizeof(small_out), false));
  EXPECT_STREQ("/mem/", small_out);

  // 3. Missing file caches empty result; second lookup returns 0 from cache.
  out[0] = 'x';
  EXPECT_EQ(0, pj_find_file(ctx.get(), "absent.txt", out, sizeof(out), true));
  EXPECT_EQ('\0', out[0]);
  out[0] = 'x';
  EXPECT_EQ(0, pj_find_file(ctx.get(), "absent.txt", out, sizeof(out), false));
  EXPECT_EQ('\0', out[0]);

  // 4. Updating search_paths clears lookupedFiles cache.
  const char* dir2 = "/mem/find_dir2";
  proj_context_set_search_paths(ctx.get(), 1, &dir2);
  EXPECT_EQ(0, pj_find_file(ctx.get(), "found.txt", out, sizeof(out), false));
}

TEST(FileManagerTest, LoadIniParsesKeysSizesTmercAlgoAndEdgeFiles) {
  ScopedEnvVar env_skip("PROJ_SKIP_READ_USER_WRITABLE_DIRECTORY");
  ScopedEnvVar env_data("PROJ_DATA");
  ScopedEnvVar env_lib("PROJ_LIB");
  ScopedEnvVar env_net("PROJ_NETWORK");
  ScopedEnvVar env_ep("PROJ_NETWORK_ENDPOINT");
  ScopedEnvVar env_ca1("PROJ_CURL_CA_BUNDLE");
  ScopedEnvVar env_ca2("CURL_CA_BUNDLE");
  ScopedEnvVar env_ca3("SSL_CERT_FILE");
  ScopedEnvVar env_best("PROJ_ONLY_BEST_DEFAULT");
  ScopedEnvVar env_native("PROJ_NATIVE_CA");

  env_skip.Set("1");
  env_data.Unset();
  env_lib.Unset();
  env_net.Unset();
  env_ep.Unset();
  env_ca1.Unset();
  env_ca2.Unset();
  env_ca3.Unset();
  env_best.Unset();
  env_native.Unset();

  struct IniCase {
    std::string_view ini_text;
    std::string_view expected_endpoint;
    bool expected_network;
    bool expected_cache_enabled;
    long long expected_cache_size;  // NOLINT(runtime/int)
    int expected_cache_ttl;
    TMercAlgo expected_tmerc;
    std::string_view expected_ca;
    bool expected_only_best;
    bool expected_native_ca;
    bool expect_tmerc_error_log;
  };

  constexpr IniCase kCases[] = {
      {
          .ini_text = "# Comment without equals\r\n"
                      "   =   \r\n"
                      "  cdn_endpoint = https://cdn.example.one  \r\n"
                      "network = YES\r\n"
                      "cache_enabled = OFF\r\n"
                      "cache_size_MB = 64\r\n"
                      "cache_ttl_sec = 3600\r\n"
                      "tmerc_default_algo = auto\r\n"
                      "ca_bundle_path = /mem/ca1.pem\r\n"
                      "only_best_default = TRUE\r\n"
                      "native_ca = ON\r\n",
          .expected_endpoint = "https://cdn.example.one",
          .expected_network = true,
          .expected_cache_enabled = false,
          .expected_cache_size = 64LL * 1024 * 1024,
          .expected_cache_ttl = 3600,
          .expected_tmerc = TMercAlgo::AUTO,
          .expected_ca = "/mem/ca1.pem",
          .expected_only_best = true,
          .expected_native_ca = true,
          .expect_tmerc_error_log = false,
      },
      {
          .ini_text = "network = ON\n"
                      "cache_enabled = TRUE\n"
                      "cache_size_MB = 0\n"
                      "tmerc_default_algo = evenden_snyder\n"
                      "only_best_default = OFF\n"
                      "native_ca = YES\n",
          .expected_endpoint = "",
          .expected_network = true,
          .expected_cache_enabled = true,
          .expected_cache_size = -1,
          .expected_cache_ttl = 86400,
          .expected_tmerc = TMercAlgo::EVENDEN_SNYDER,
          .expected_ca = "",
          .expected_only_best = false,
          .expected_native_ca = true,
          .expect_tmerc_error_log = false,
      },
      {
          .ini_text = "network = TRUE\n"
                      "cache_enabled = YES\n"
                      "cache_size_MB = -5\n"
                      "tmerc_default_algo = poder_engsager\n"
                      "only_best_default = YES\n"
                      "native_ca = TRUE\n",
          .expected_endpoint = "",
          .expected_network = true,
          .expected_cache_enabled = true,
          .expected_cache_size = -1,
          .expected_cache_ttl = 86400,
          .expected_tmerc = TMercAlgo::PODER_ENGSAGER,
          .expected_ca = "",
          .expected_only_best = true,
          .expected_native_ca = true,
          .expect_tmerc_error_log = false,
      },
      {
          .ini_text =
              "network = OFF\n"
              "cache_enabled = ON\n"
              "tmerc_default_algo = bogus_algo\n"
              "only_best_default = ON\n"
              "native_ca = OFF\n"
              "unknown_ini_key = ignored\n"
              "cache_ttl_sec = 120",  // intentionally no trailing newline
          .expected_endpoint = "",
          .expected_network = false,
          .expected_cache_enabled = true,
          .expected_cache_size = 300LL * 1024 * 1024,
          .expected_cache_ttl = 120,
          .expected_tmerc = TMercAlgo::PODER_ENGSAGER,
          .expected_ca = "",
          .expected_only_best = true,
          .expected_native_ca = false,
          .expect_tmerc_error_log = true,
      },
  };

  for (size_t i = 0; i < std::size(kCases); ++i) {
    const auto& tc = kCases[i];
    InMemoryFileSystem fs;
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ASSERT_TRUE(fs.Install(ctx.get()));
    proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
    const char* dir = "/mem/ini";
    proj_context_set_search_paths(ctx.get(), 1, &dir);
    fs.AddTextFile("/mem/ini/proj.ini", tc.ini_text);

    ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
    pj_load_ini(ctx.get());
    // Second call is an immediate no-op.
    pj_load_ini(ctx.get());

    EXPECT_TRUE(ctx->iniFileLoaded) << "case " << i;
    EXPECT_STREQ(std::string(tc.expected_endpoint).c_str(),
                 proj_context_get_url_endpoint(ctx.get()))
        << "case " << i;
    EXPECT_EQ(tc.expected_network, ctx->networking.enabled) << "case " << i;
    EXPECT_EQ(tc.expected_cache_enabled, ctx->gridChunkCache.enabled)
        << "case " << i;
    EXPECT_EQ(tc.expected_cache_size, ctx->gridChunkCache.max_size)
        << "case " << i;
    EXPECT_EQ(tc.expected_cache_ttl, ctx->gridChunkCache.ttl) << "case " << i;
    EXPECT_EQ(tc.expected_tmerc, ctx->defaultTmercAlgo) << "case " << i;
    EXPECT_EQ(tc.expected_ca, ctx->ca_bundle_path) << "case " << i;
    EXPECT_EQ(tc.expected_only_best,
              ctx->errorIfBestTransformationNotAvailableDefault)
        << "case " << i;
    EXPECT_EQ(tc.expected_native_ca, ctx->native_ca) << "case " << i;
    EXPECT_EQ(tc.expect_tmerc_error_log,
              capture.ContainsMessage("Invalid value for tmerc_default_algo"))
        << "case " << i;
  }

  // Missing, empty, oversized (> 100 KiB), and short-read proj.ini files
  // complete cleanly with defaults intact.
  {
    InMemoryFileSystem fs;
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ASSERT_TRUE(fs.Install(ctx.get()));
    proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
    const char* dir = "/mem/ini_missing";
    proj_context_set_search_paths(ctx.get(), 1, &dir);
    pj_load_ini(ctx.get());
    EXPECT_TRUE(ctx->iniFileLoaded);
    EXPECT_THAT(ctx->endpoint, IsEmpty());
  }
  {
    InMemoryFileSystem fs;
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ASSERT_TRUE(fs.Install(ctx.get()));
    proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
    const char* dir = "/mem/ini_empty";
    proj_context_set_search_paths(ctx.get(), 1, &dir);
    fs.AddTextFile("/mem/ini_empty/proj.ini", "");
    pj_load_ini(ctx.get());
    EXPECT_TRUE(ctx->iniFileLoaded);
    EXPECT_THAT(ctx->endpoint, IsEmpty());
  }
  {
    InMemoryFileSystem fs;
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ASSERT_TRUE(fs.Install(ctx.get()));
    proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
    const char* dir = "/mem/ini_big";
    proj_context_set_search_paths(ctx.get(), 1, &dir);
    fs.AddTextFile("/mem/ini_big/proj.ini",
                   std::string(100u * 1024u + 1u, 'x'));
    pj_load_ini(ctx.get());
    EXPECT_TRUE(ctx->iniFileLoaded);
    EXPECT_THAT(ctx->endpoint, IsEmpty());
  }
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ShortReadState state;
    PROJ_FILE_API api = MakeValidNoOpFileApi();
    api.open_cbk = &ShortReadOpen;
    api.read_cbk = &ShortReadRead;
    api.seek_cbk = &ShortReadSeek;
    api.tell_cbk = &ShortReadTell;
    ASSERT_EQ(1, proj_context_set_fileapi(ctx.get(), &api, &state));
    proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
    const char* dir = "/mem/short_ini";
    proj_context_set_search_paths(ctx.get(), 1, &dir);
    pj_load_ini(ctx.get());
    EXPECT_TRUE(ctx->iniFileLoaded);
    EXPECT_THAT(ctx->endpoint, IsEmpty());
  }
}

TEST(FileManagerTest, LoadIniEnvVarsOverrideIniAndCaBundleFallbackOrder) {
  ScopedEnvVar env_skip("PROJ_SKIP_READ_USER_WRITABLE_DIRECTORY");
  ScopedEnvVar env_data("PROJ_DATA");
  ScopedEnvVar env_lib("PROJ_LIB");
  ScopedEnvVar env_net("PROJ_NETWORK");
  ScopedEnvVar env_ep("PROJ_NETWORK_ENDPOINT");
  ScopedEnvVar env_ca1("PROJ_CURL_CA_BUNDLE");
  ScopedEnvVar env_ca2("CURL_CA_BUNDLE");
  ScopedEnvVar env_ca3("SSL_CERT_FILE");
  ScopedEnvVar env_best("PROJ_ONLY_BEST_DEFAULT");
  ScopedEnvVar env_native("PROJ_NATIVE_CA");

  env_skip.Set("1");
  env_data.Unset();
  env_lib.Unset();

  InMemoryFileSystem fs;
  const auto run_with_ini = [&fs](std::string_view ini_text) {
    ScopedContext ctx = MakeContext();
    EXPECT_NE(nullptr, ctx.get());
    if (!ctx) return ctx;
    EXPECT_TRUE(fs.Install(ctx.get()));
    proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
    const char* dir = "/mem/ini_env";
    proj_context_set_search_paths(ctx.get(), 1, &dir);
    fs.AddTextFile("/mem/ini_env/proj.ini", ini_text);
    pj_load_ini(ctx.get());
    return ctx;
  };

  // 1. Env vars override conflicting proj.ini values, and PROJ_CURL_CA_BUNDLE
  //    wins over CURL_CA_BUNDLE and SSL_CERT_FILE.
  env_net.Set("OFF");
  env_ep.Set("https://env.endpoint.test");
  env_ca1.Set("/env/proj_ca.pem");
  env_ca2.Set("/env/curl_ca.pem");
  env_ca3.Set("/env/ssl_cert.pem");
  env_best.Set("YES");
  env_native.Set("TRUE");
  {
    ScopedContext ctx = run_with_ini(
        "network = ON\n"
        "cdn_endpoint = https://ini.ignored\n"
        "ca_bundle_path = /ini/ignored\n"
        "only_best_default = OFF\n"
        "native_ca = OFF\n");
    ASSERT_NE(nullptr, ctx.get());
    EXPECT_FALSE(ctx->networking.enabled);
    EXPECT_STREQ("https://env.endpoint.test",
                 proj_context_get_url_endpoint(ctx.get()));
    EXPECT_EQ("/env/proj_ca.pem", ctx->ca_bundle_path);
    EXPECT_TRUE(ctx->errorIfBestTransformationNotAvailableDefault);
    EXPECT_FALSE(ctx->warnIfBestTransformationNotAvailableDefault);
    EXPECT_TRUE(ctx->native_ca);
  }

  // 2. CURL_CA_BUNDLE used when PROJ_CURL_CA_BUNDLE is unset; empty strings
  //    for PROJ_NETWORK / PROJ_NATIVE_CA let proj.ini values take effect.
  env_net.Set("");
  env_ep.Set("");
  env_ca1.Unset();
  env_ca2.Set("/env/curl_ca.pem");
  env_ca3.Set("/env/ssl_cert.pem");
  env_best.Set("TRUE");
  env_native.Set("");
  {
    ScopedContext ctx = run_with_ini("network = ON\nnative_ca = YES\n");
    ASSERT_NE(nullptr, ctx.get());
    EXPECT_TRUE(ctx->networking.enabled);
    EXPECT_EQ("/env/curl_ca.pem", ctx->ca_bundle_path);
    EXPECT_TRUE(ctx->errorIfBestTransformationNotAvailableDefault);
    EXPECT_TRUE(ctx->native_ca);
  }

  // 3. SSL_CERT_FILE used when both PROJ_CURL_CA_BUNDLE and CURL_CA_BUNDLE
  //    are unset; PROJ_NETWORK="ON"/"TRUE", PROJ_ONLY_BEST_DEFAULT="OFF", and
  //    PROJ_NATIVE_CA="OFF" exercise remaining env branches.
  env_ca2.Unset();
  env_ca3.Set("/env/ssl_cert.pem");
  env_net.Set("ON");
  env_best.Set("ON");
  env_native.Set("YES");
  {
    ScopedContext ctx = run_with_ini("");
    ASSERT_NE(nullptr, ctx.get());
    EXPECT_TRUE(ctx->networking.enabled);
    EXPECT_EQ("/env/ssl_cert.pem", ctx->ca_bundle_path);
    EXPECT_TRUE(ctx->errorIfBestTransformationNotAvailableDefault);
    EXPECT_TRUE(ctx->native_ca);
  }
  env_net.Set("TRUE");
  env_best.Set("OFF");
  env_native.Set("OFF");
  {
    ScopedContext ctx =
        run_with_ini("only_best_default = ON\nnative_ca = ON\n");
    ASSERT_NE(nullptr, ctx.get());
    EXPECT_TRUE(ctx->networking.enabled);
    EXPECT_FALSE(ctx->errorIfBestTransformationNotAvailableDefault);
    EXPECT_FALSE(ctx->warnIfBestTransformationNotAvailableDefault);
    EXPECT_FALSE(ctx->native_ca);
  }

  // 4. proj_context_set_ca_bundle_path loads ini first, then overrides/clears.
  env_ca3.Unset();
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ASSERT_TRUE(fs.Install(ctx.get()));
    proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
    const char* dir = "/mem/ca_set";
    proj_context_set_search_paths(ctx.get(), 1, &dir);
    fs.AddTextFile("/mem/ca_set/proj.ini", "ca_bundle_path = /ini/ca.pem\n");
    proj_context_set_ca_bundle_path(ctx.get(), "/override/ca.pem");
    EXPECT_TRUE(ctx->iniFileLoaded);
    EXPECT_EQ("/override/ca.pem", ctx->ca_bundle_path);
    proj_context_set_ca_bundle_path(ctx.get(), nullptr);
    EXPECT_THAT(ctx->ca_bundle_path, IsEmpty());
  }
}

TEST(FileManagerTest, NullContextFallsBackToDefaultContext) {
  ScopedEnvVar env_skip("PROJ_SKIP_READ_USER_WRITABLE_DIRECTORY");
  ScopedEnvVar env_data("PROJ_DATA");
  ScopedEnvVar env_lib("PROJ_LIB");
  ScopedEnvVar env_net("PROJ_NETWORK");
  ScopedEnvVar env_ep("PROJ_NETWORK_ENDPOINT");
  ScopedEnvVar env_ca1("PROJ_CURL_CA_BUNDLE");
  ScopedEnvVar env_ca2("CURL_CA_BUNDLE");
  ScopedEnvVar env_ca3("SSL_CERT_FILE");
  ScopedEnvVar env_best("PROJ_ONLY_BEST_DEFAULT");
  ScopedEnvVar env_native("PROJ_NATIVE_CA");
  env_skip.Set("1");
  env_data.Unset();
  env_lib.Unset();
  env_net.Unset();
  env_ep.Unset();
  env_ca1.Unset();
  env_ca2.Unset();
  env_ca3.Unset();
  env_best.Unset();
  env_native.Unset();

  PJ_CONTEXT* def = pj_get_default_ctx();
  ASSERT_NE(nullptr, def);

  const auto saved_file_api = def->fileApi;
  const auto saved_file_finder = def->file_finder;
  void* const saved_file_finder_data = def->file_finder_user_data;
  const auto saved_search_paths = def->search_paths;
  const auto saved_lookuped_files = def->lookupedFiles;
  const std::string saved_uwd = def->user_writable_directory;
  const std::string saved_env_proj_data = def->env_var_proj_data;
  const std::string saved_vfs = def->custom_sqlite3_vfs_name;
  const bool saved_ini_loaded = def->iniFileLoaded;
  const std::string saved_endpoint = def->endpoint;
  const auto saved_networking = def->networking;
  const std::string saved_ca_bundle = def->ca_bundle_path;
  const bool saved_native_ca = def->native_ca;
  const auto saved_cache = def->gridChunkCache;
  const auto saved_tmerc = def->defaultTmercAlgo;
  const bool saved_err_best = def->errorIfBestTransformationNotAvailableDefault;
  const bool saved_warn_best = def->warnIfBestTransformationNotAvailableDefault;

  InMemoryFileSystem fs;
  absl::Cleanup restore_default = [&] {
    proj_cleanup();
    def->fileApi = saved_file_api;
    def->file_finder = saved_file_finder;
    def->file_finder_user_data = saved_file_finder_data;
    def->set_search_paths(saved_search_paths);
    def->lookupedFiles = saved_lookuped_files;
    def->user_writable_directory = saved_uwd;
    def->env_var_proj_data = saved_env_proj_data;
    def->custom_sqlite3_vfs_name = saved_vfs;
    def->iniFileLoaded = saved_ini_loaded;
    def->endpoint = saved_endpoint;
    def->networking = saved_networking;
    def->ca_bundle_path = saved_ca_bundle;
    def->native_ca = saved_native_ca;
    def->gridChunkCache = saved_cache;
    def->defaultTmercAlgo = saved_tmerc;
    def->errorIfBestTransformationNotAvailableDefault = saved_err_best;
    def->warnIfBestTransformationNotAvailableDefault = saved_warn_best;
    proj_errno_reset(nullptr);
  };

  def->iniFileLoaded = false;
  def->endpoint.clear();
  ASSERT_TRUE(fs.Install(nullptr));
  proj_context_set_file_finder(nullptr, nullptr, nullptr);
  const char* sp = "/mem/null_ctx_sp";
  proj_context_set_search_paths(nullptr, 1, &sp);
  proj_context_set_sqlite3_vfs_name(nullptr, "null_ctx_vfs");
  proj_context_set_user_writable_directory(nullptr, "/mem/null_ctx_uwd", FALSE);
  EXPECT_STREQ("/mem/null_ctx_uwd",
               proj_context_get_user_writable_directory(nullptr, FALSE));

  proj_context_set_ca_bundle_path(nullptr, "/mem/null_ctx_ca.pem");
  EXPECT_EQ("/mem/null_ctx_ca.pem", def->ca_bundle_path);
  EXPECT_STREQ("", proj_context_get_url_endpoint(nullptr));
  proj_context_set_url_endpoint(nullptr, "https://null.endpoint.test");
  EXPECT_STREQ("https://null.endpoint.test",
               proj_context_get_url_endpoint(nullptr));

  fs.AddTextFile("/mem/null_ctx_sp/ok.txt", "ok");
  EXPECT_THAT(NS_PROJ::FileManager::open_resource_file(nullptr, "ok.txt"),
              NotNull());
  EXPECT_EQ(nullptr,
            NS_PROJ::FileManager::open_resource_file(nullptr, "/mem/absent.txt")
                .get());
}

class ScopedNetworkWrapper {
 public:
  explicit ScopedNetworkWrapper(bool enable = true)
      : prev_(g_use_context_network_flag.exchange(enable,
                                                  std::memory_order_relaxed)) {}

  ~ScopedNetworkWrapper() {
    g_use_context_network_flag.store(prev_, std::memory_order_relaxed);
  }

  ScopedNetworkWrapper(const ScopedNetworkWrapper&) = delete;
  ScopedNetworkWrapper& operator=(const ScopedNetworkWrapper&) = delete;

 private:
  bool prev_;
};

struct MockNetState {
  int dummy_handle = 1;
};

PROJ_NETWORK_HANDLE* MockNetOpen(PJ_CONTEXT*, const char* url,
                                 // NOLINTNEXTLINE(runtime/int)
                                 unsigned long long, size_t size_to_read,
                                 void* buffer, size_t* out_size_read,
                                 size_t error_string_max_size,
                                 char* out_error_string, void* user_data) {
  if (url == nullptr || user_data == nullptr ||
      std::string_view(url).find("fail") != std::string_view::npos) {
    if (out_size_read != nullptr) {
      *out_size_read = 0;
    }
    if (out_error_string != nullptr && error_string_max_size > 0) {
      std::snprintf(out_error_string, error_string_max_size, "mock 404");
    }
    return nullptr;
  }
  constexpr std::string_view kPayload = "grid";
  const size_t n = std::min(size_to_read, kPayload.size());
  if (buffer != nullptr && n > 0) {
    std::memcpy(buffer, kPayload.data(), n);
  }
  if (out_size_read != nullptr) {
    *out_size_read = n;
  }
  if (out_error_string != nullptr && error_string_max_size > 0) {
    out_error_string[0] = '\0';
  }
  return reinterpret_cast<PROJ_NETWORK_HANDLE*>(
      &(static_cast<MockNetState*>(user_data)->dummy_handle));
}

void MockNetClose(PJ_CONTEXT*, PROJ_NETWORK_HANDLE*, void*) {}

const char* MockNetGetHeaderValue(PJ_CONTEXT*, PROJ_NETWORK_HANDLE*,
                                  const char* header_name, void*) {
  if (header_name == nullptr) {
    return nullptr;
  }
  const std::string_view name(header_name);
  if (name == "Content-Range") {
    return "bytes 0-3/4";
  }
  if (name == "Last-Modified") {
    return "Mon, 01 Jan 2026 00:00:00 GMT";
  }
  if (name == "ETag") {
    return "\"etag1\"";
  }
  return nullptr;
}

// NOLINTNEXTLINE(runtime/int)
size_t MockNetReadRange(PJ_CONTEXT*, PROJ_NETWORK_HANDLE*, unsigned long long,
                        size_t, void*, size_t, char*, void*) {
  return 0;
}

PROJ_FILE_HANDLE* ThrowingOpen(PJ_CONTEXT*, const char*, PROJ_OPEN_ACCESS,
                               void*) {
  throw std::runtime_error("simulated open_cbk failure");
}

TEST(FileManagerTest, OpenResourceFileHandlesSearchPathAndOpenExceptions) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  NoOpFileApiState state;
  PROJ_FILE_API api = MakeValidNoOpFileApi();
  api.open_cbk = &ThrowingOpen;
  ASSERT_EQ(1, proj_context_set_fileapi(ctx.get(), &api, &state));
  proj_context_set_file_finder(ctx.get(), nullptr, nullptr);

  // Setting search_paths causes open_file() to throw inside the search_paths
  // loop (caught by the inner catch block) and then throw again on the final
  // fallback open_file() call (caught by the outer catch block).
  const char* dir = "/mem/throw_sp";
  proj_context_set_search_paths(ctx.get(), 1, &dir);

  ScopedLogCapture capture(ctx.get(), PJ_LOG_DEBUG);
  EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                         ctx.get(), "throw_target.dat")
                         .get());
  EXPECT_TRUE(capture.ContainsMessage("out of memory"));
}

TEST(FileManagerTest, OpenResourceFileNetworkFallbackAndFindFileNetworkToggle) {
  absl::Cleanup cleanup = [] {
    NS_PROJ::FileManager::clearMemoryCache();
    proj_cleanup();
  };
  ScopedNetworkWrapper enable_net(true);
  ScopedEnvVar env_skip("PROJ_SKIP_READ_USER_WRITABLE_DIRECTORY");
  ScopedEnvVar env_data("PROJ_DATA");
  ScopedEnvVar env_lib("PROJ_LIB");
  ScopedEnvVar env_net("PROJ_NETWORK");
  ScopedEnvVar env_ep("PROJ_NETWORK_ENDPOINT");
  env_skip.Set("1");
  env_data.Unset();
  env_lib.Unset();
  env_net.Unset();
  env_ep.Unset();

  InMemoryFileSystem fs;
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ASSERT_TRUE(fs.Install(ctx.get()));
  proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
  proj_grid_cache_set_enable(ctx.get(), FALSE);

  MockNetState net_state;
  ASSERT_EQ(1, proj_context_set_network_callbacks(
                   ctx.get(), &MockNetOpen, &MockNetClose,
                   &MockNetGetHeaderValue, &MockNetReadRange, &net_state));
  ASSERT_EQ(1, proj_context_set_enable_network(ctx.get(), TRUE));
  ASSERT_EQ(1, proj_context_is_network_enabled(ctx.get()));

  // 1. Direct FileManager::open on an https:// URL delegates to
  //    pj_network_file_open when networking is enabled.
  {
    auto direct_net =
        NS_PROJ::FileManager::open(ctx.get(), "https://cdn.proj.org/direct.tif",
                                   NS_PROJ::FileAccess::READ_ONLY);
    EXPECT_THAT(direct_net, NotNull());
  }

  // 2. Known grid ("conus" -> "us_noaa_conus.tif") with empty endpoint uses
  //    the https://cdn.proj.org/ URL from proj.db and caches grid info on
  //    DatabaseContext.
  char resolved[256] = {};
  proj_context_set_url_endpoint(ctx.get(), "");
  proj_context_errno_set(ctx.get(), ENOENT);
  {
    ScopedLogCapture capture(ctx.get(), PJ_LOG_DEBUG);
    auto f = NS_PROJ::FileManager::open_resource_file(
        ctx.get(), "conus", resolved, sizeof(resolved));
    ASSERT_THAT(f, NotNull());
    EXPECT_STREQ("https://cdn.proj.org/us_noaa_conus.tif", resolved);
    EXPECT_EQ(0, proj_context_errno(ctx.get()));
    EXPECT_TRUE(capture.ContainsMessage(
        "Using https://cdn.proj.org/us_noaa_conus.tif"));
  }

  // 3. Setting a custom endpoint on the same context (without and with a
  //    trailing slash) rewrites the cached https://cdn.proj.org/ URL inside
  //    open_resource_file.
  NS_PROJ::FileManager::clearMemoryCache();
  proj_context_set_url_endpoint(ctx.get(), "https://mirror.example.test");
  {
    auto f = NS_PROJ::FileManager::open_resource_file(
        ctx.get(), "conus", resolved, sizeof(resolved));
    ASSERT_THAT(f, NotNull());
    EXPECT_STREQ("https://mirror.example.test/us_noaa_conus.tif", resolved);
  }

  NS_PROJ::FileManager::clearMemoryCache();
  proj_context_set_url_endpoint(ctx.get(), "https://mirror.example.test/");
  {
    auto f = NS_PROJ::FileManager::open_resource_file(ctx.get(), "conus",
                                                      nullptr, 0);
    EXPECT_THAT(f, NotNull());
  }

  // 4. Unlisted grid name not in proj.db falls back to constructing the remote
  //    URL from proj_context_get_url_endpoint (without trailing slash, with
  //    trailing slash on failed remote open, and with empty endpoint).
  NS_PROJ::FileManager::clearMemoryCache();
  proj_context_set_url_endpoint(ctx.get(), "https://fallback.example.test");
  {
    auto f = NS_PROJ::FileManager::open_resource_file(
        ctx.get(), "custom_unlisted_grid.tif", resolved, sizeof(resolved));
    ASSERT_THAT(f, NotNull());
    EXPECT_STREQ("https://fallback.example.test/custom_unlisted_grid.tif",
                 resolved);
  }

  proj_context_set_url_endpoint(ctx.get(), "https://fallback.example.test/");
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                           ctx.get(), "fail_unlisted_grid.tif", resolved,
                           sizeof(resolved))
                           .get());
  }

  proj_context_set_url_endpoint(ctx.get(), "");
  EXPECT_EQ(nullptr, NS_PROJ::FileManager::open_resource_file(
                         ctx.get(), "unlisted_no_endpoint.tif")
                         .get());

  // 5. pj_find_file with disable_network=true temporarily disables networking
  //    during open_resource_file and restores it afterwards.
  proj_context_set_url_endpoint(ctx.get(), "https://fallback.example.test");
  EXPECT_EQ(0, pj_find_file(ctx.get(), "find_net_disabled.tif", resolved,
                            sizeof(resolved), /*disable_network=*/true));
  EXPECT_EQ(1, proj_context_is_network_enabled(ctx.get()));

  // 6. When DatabaseContext::lookForGridInfo throws (minimal SQLite DB without
  //    grid_alternatives, queried with "proj.db" so earlier non-.tif/.tif
  //    branches are skipped), open_resource_file catches the exception,
  //    re-enables networking, and returns nullptr.
  {
    const char* major_ptr = proj_context_get_database_metadata(
        ctx.get(), "DATABASE.LAYOUT.VERSION.MAJOR");
    ASSERT_NE(nullptr, major_ptr);
    const std::string major(major_ptr);
    const char* minor_ptr = proj_context_get_database_metadata(
        ctx.get(), "DATABASE.LAYOUT.VERSION.MINOR");
    ASSERT_NE(nullptr, minor_ptr);
    const std::string minor(minor_ptr);

    TempFile db_file("minimal_net_no_grid_alt.db");
    sqlite3* raw_db = nullptr;
    const int open_rc =
        sqlite3_open_v2(db_file.c_str(), &raw_db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    auto close_db = [](sqlite3* db) { sqlite3_close(db); };
    std::unique_ptr<sqlite3, decltype(close_db)> db(raw_db, close_db);
    ASSERT_EQ(SQLITE_OK, open_rc);

    ASSERT_EQ(
        SQLITE_OK,
        sqlite3_exec(db.get(), "CREATE TABLE metadata (key TEXT, value TEXT);",
                     nullptr, nullptr, nullptr));
    const auto insert_meta = [&](const char* k, const std::string& v) {
      sqlite3_stmt* stmt = nullptr;
      ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                               db.get(), "INSERT INTO metadata VALUES (?, ?);",
                               -1, &stmt, nullptr));
      absl::Cleanup finalize = [stmt] { sqlite3_finalize(stmt); };
      ASSERT_EQ(SQLITE_OK, sqlite3_bind_text(stmt, 1, k, -1, nullptr));
      ASSERT_EQ(SQLITE_OK, sqlite3_bind_text(stmt, 2, v.c_str(), -1, nullptr));
      ASSERT_EQ(SQLITE_DONE, sqlite3_step(stmt));
    };
    insert_meta("DATABASE.LAYOUT.VERSION.MAJOR", major);
    insert_meta("DATABASE.LAYOUT.VERSION.MINOR", minor);
    db.reset();

    ScopedContext min_ctx = MakeContext();
    ASSERT_NE(nullptr, min_ctx.get());
    ASSERT_EQ(1, proj_context_set_database_path(min_ctx.get(), db_file.c_str(),
                                                nullptr, nullptr));
    ASSERT_TRUE(fs.Install(min_ctx.get()));
    proj_context_set_file_finder(min_ctx.get(), nullptr, nullptr);
    ASSERT_EQ(1, proj_context_set_network_callbacks(
                     min_ctx.get(), &MockNetOpen, &MockNetClose,
                     &MockNetGetHeaderValue, &MockNetReadRange, &net_state));
    ASSERT_EQ(1, proj_context_set_enable_network(min_ctx.get(), TRUE));

    ScopedLogCapture capture(min_ctx.get(), PJ_LOG_DEBUG);
    EXPECT_EQ(nullptr,
              NS_PROJ::FileManager::open_resource_file(min_ctx.get(), "proj.db")
                  .get());
    EXPECT_TRUE(capture.ContainsMessage("grid_alternatives"));
    EXPECT_EQ(1, proj_context_is_network_enabled(min_ctx.get()));
  }
}

// TODO(schwehr): Unreachable lines in google3 default builds remain in:
//   - PROJ_DATA macro / PROJ_DATA_ENV_VAR_TRIED_LAST compile-time branches,
//     EMBED_RESOURCE_FILES, and _WIN32 blocks.
//   - pj_get_relative_share_proj_internal_* / get_path_from_relative_share_proj
//     when ../share/proj does not exist in the test sandbox.

}  // namespace
}  // namespace proj
