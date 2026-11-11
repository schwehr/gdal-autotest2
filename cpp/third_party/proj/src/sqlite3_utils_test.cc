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
// Unit tests for PROJ's SQLite3 custom VFS, network VFS, and SQLiteStatement
// utilities (proj/src/sqlite3_utils.{hpp,cpp}). Note: EMBED_RESOURCE_FILES is
// not defined in google3 builds (proj.db is embedded via google3_proj4_hooks),
// so SQLite3VFS::createMem() is compiled out and excluded from coverage.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/sqlite3_utils.hpp
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/sqlite3_utils.cpp

#include "third_party/proj4/proj/src/sqlite3_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/google/util/temp_file.h"
#include "third_party/proj4/proj/src/filemanager.hpp"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/sqlite/sqlite3.h"

namespace proj {
namespace {

using ::testing::IsNull;
using ::testing::NotNull;

struct SqliteDbDeleter {
  void operator()(sqlite3* db) const noexcept { sqlite3_close(db); }
};
using ScopedSqliteDb = std::unique_ptr<sqlite3, SqliteDbDeleter>;

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream ifs(path, std::ios::binary | std::ios::ate);
  if (!ifs) {
    return {};
  }
  const std::ifstream::pos_type size = ifs.tellg();
  if (size <= 0) {
    return {};
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  ifs.seekg(0, std::ios::beg);
  ifs.read(reinterpret_cast<char*>(bytes.data()), size);
  return ifs ? bytes : std::vector<uint8_t>{};
}

// Helper to create a minimal on-disk SQLite database using the default VFS.
void CreateSampleDatabase(const std::filesystem::path& db_path) {
  sqlite3* raw_db = nullptr;
  const int open_rc =
      sqlite3_open_v2(db_path.c_str(), &raw_db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  ScopedSqliteDb db(raw_db);
  ASSERT_EQ(SQLITE_OK, open_rc);
  ASSERT_THAT(db.get(), NotNull());

  // Use DELETE journal mode so no -shm/-wal sidecar files remain on disk.
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_exec(db.get(),
                   "PRAGMA journal_mode=DELETE;"
                   "CREATE TABLE items(id INTEGER PRIMARY KEY, label TEXT);"
                   "INSERT INTO items(id, label) VALUES(42, 'alpha');",
                   nullptr, nullptr, nullptr));
}

struct MockNetworkHandle {
  const std::vector<uint8_t>* bytes;
  const std::string* url;
  const std::string* content_range;
  const std::string* last_modified;
  const std::string* etag;
};

struct MockNetworkState {
  std::vector<uint8_t> bytes;
  std::string expected_url;
  std::string content_range;
  std::string last_modified;
  std::string etag;
  int open_calls = 0;
  int read_range_calls = 0;
  int close_calls = 0;

  static void WriteError(size_t max_size, char* out, std::string_view msg) {
    if (out != nullptr && max_size > 0) {
      const size_t n = std::min(msg.size(), max_size - 1);
      std::memcpy(out, msg.data(), n);
      out[n] = '\0';
    }
  }

  static PROJ_NETWORK_HANDLE* Open(PJ_CONTEXT*, const char* url,
                                   // NOLINTNEXTLINE(runtime/int)
                                   unsigned long long offset,
                                   size_t size_to_read, void* buffer,
                                   size_t* out_size_read,
                                   size_t error_string_max_size,
                                   char* out_error_string, void* user_data) {
    auto* state = static_cast<MockNetworkState*>(user_data);
    ++state->open_calls;
    if (url == nullptr || state->expected_url != url) {
      WriteError(error_string_max_size, out_error_string, "Not found");
      if (out_size_read != nullptr) {
        *out_size_read = 0;
      }
      return nullptr;
    }
    size_t n = 0;
    if (offset < state->bytes.size() && buffer != nullptr) {
      n = std::min<size_t>(size_to_read,
                           state->bytes.size() - static_cast<size_t>(offset));
      std::memcpy(buffer, state->bytes.data() + offset, n);
    }
    if (out_size_read != nullptr) {
      *out_size_read = n;
    }
    return reinterpret_cast<PROJ_NETWORK_HANDLE*>(
        new MockNetworkHandle{.bytes = &state->bytes,
                              .url = &state->expected_url,
                              .content_range = &state->content_range,
                              .last_modified = &state->last_modified,
                              .etag = &state->etag});
  }

  static void Close(PJ_CONTEXT*, PROJ_NETWORK_HANDLE* handle, void* user_data) {
    auto* state = static_cast<MockNetworkState*>(user_data);
    ++state->close_calls;
    delete reinterpret_cast<MockNetworkHandle*>(handle);
  }

  static const char* GetHeaderValue(PJ_CONTEXT*, PROJ_NETWORK_HANDLE* handle,
                                    const char* header_name, void*) {
    if (handle == nullptr || header_name == nullptr) {
      return nullptr;
    }
    auto* h = reinterpret_cast<MockNetworkHandle*>(handle);
    const std::string_view name(header_name);
    if (name == "Content-Range") {
      return h->content_range->c_str();
    }
    if (name == "Last-Modified") {
      return h->last_modified->c_str();
    }
    if (name == "ETag") {
      return h->etag->c_str();
    }
    return nullptr;
  }

  static size_t ReadRange(PJ_CONTEXT*, PROJ_NETWORK_HANDLE* handle,
                          // NOLINTNEXTLINE(runtime/int)
                          unsigned long long offset, size_t size_to_read,
                          void* buffer, size_t error_string_max_size,
                          char* out_error_string, void* user_data) {
    auto* state = static_cast<MockNetworkState*>(user_data);
    ++state->read_range_calls;
    auto* h = reinterpret_cast<MockNetworkHandle*>(handle);
    if (h == nullptr || offset >= h->bytes->size() || buffer == nullptr) {
      WriteError(error_string_max_size, out_error_string, "EOF");
      return 0;
    }
    const size_t n = std::min<size_t>(
        size_to_read, h->bytes->size() - static_cast<size_t>(offset));
    std::memcpy(buffer, h->bytes->data() + offset, n);
    return n;
  }
};

TEST(Sqlite3UtilsTest, CreateCustomVfsFakeSyncLockAndSQLiteStatementRoundTrip) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  TempFile db_file("sqlite3_utils_custom_fake_sync_lock.db");

  auto vfs = NS_PROJ::SQLite3VFS::create(/*fakeSync=*/true, /*fakeLock=*/true,
                                         /*skipStatJournalAndWAL=*/false);
  ASSERT_THAT(vfs, NotNull());
  ASSERT_THAT(vfs->name(), NotNull());
  EXPECT_STREQ(vfs->name(), vfs->raw()->zName);
  EXPECT_EQ(vfs->raw(), sqlite3_vfs_find(vfs->name()));

  sqlite3* raw_db = nullptr;
  const int open_rc =
      sqlite3_open_v2(db_file.c_str(), &raw_db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs->name());
  ScopedSqliteDb db(raw_db);
  ASSERT_EQ(SQLITE_OK, open_rc);
  ASSERT_THAT(db.get(), NotNull());

  ASSERT_EQ(SQLITE_OK, sqlite3_exec(db.get(),
                                    "CREATE TABLE records("
                                    "  id INTEGER PRIMARY KEY,"
                                    "  name TEXT,"
                                    "  payload BLOB,"
                                    "  note TEXT);",
                                    nullptr, nullptr, nullptr));

  // 1. Insert two rows using SQLiteStatement bind* and reset(), keeping
  // SQLITE_STATIC string/blob buffers alive until after execute() and reset().
  {
    sqlite3_stmt* raw_insert = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_prepare_v2(db.get(),
                                 "INSERT INTO records(id, name, payload, note) "
                                 "VALUES (?, ?, ?, ?)",
                                 -1, &raw_insert, nullptr));
    NS_PROJ::SQLiteStatement stmt(raw_insert);

    const std::string row1_name = "first";
    const std::array<uint8_t, 4> row1_blob = {0x10, 0x20, 0x30, 0x40};
    stmt.bindInt64(101);
    stmt.bindText(row1_name.c_str());
    stmt.bindBlob(row1_blob.data(), row1_blob.size());
    stmt.bindNull();
    EXPECT_EQ(SQLITE_DONE, stmt.execute());

    stmt.reset();
    const std::string row2_name = "second";
    const std::string row2_note = "present";
    const std::array<uint8_t, 2> row2_blob = {0xAA, 0xBB};
    stmt.bindInt64(202);
    stmt.bindText(row2_name.c_str());
    stmt.bindBlob(row2_blob.data(), row2_blob.size());
    stmt.bindText(row2_note.c_str());
    EXPECT_EQ(SQLITE_DONE, stmt.execute());
  }

  // 2. Query rows back and verify getInt64(), getText(), getBlob(), and
  // resetResIndex().
  {
    sqlite3_stmt* raw_select = nullptr;
    ASSERT_EQ(
        SQLITE_OK,
        sqlite3_prepare_v2(
            db.get(), "SELECT id, name, payload, note FROM records ORDER BY id",
            -1, &raw_select, nullptr));
    NS_PROJ::SQLiteStatement stmt(raw_select);

    ASSERT_EQ(SQLITE_ROW, stmt.execute());
    EXPECT_EQ(101, stmt.getInt64());
    EXPECT_STREQ("first", stmt.getText());
    int blob_size = -1;
    const void* blob_ptr = stmt.getBlob(blob_size);
    ASSERT_EQ(4, blob_size);
    ASSERT_THAT(blob_ptr, NotNull());
    const std::array<uint8_t, 4> expected_row1_blob = {0x10, 0x20, 0x30, 0x40};
    EXPECT_EQ(0, std::memcmp(expected_row1_blob.data(), blob_ptr, 4));
    EXPECT_THAT(stmt.getText(), IsNull());

    // Rewind column index back to column 0 for the current row.
    stmt.resetResIndex();
    EXPECT_EQ(101, stmt.getInt64());

    // Advance to the second row using resetResIndex() between rows.
    stmt.resetResIndex();
    ASSERT_EQ(SQLITE_ROW, stmt.execute());
    EXPECT_EQ(202, stmt.getInt64());
    EXPECT_STREQ("second", stmt.getText());
    blob_ptr = stmt.getBlob(blob_size);
    ASSERT_EQ(2, blob_size);
    EXPECT_STREQ("present", stmt.getText());

    EXPECT_EQ(SQLITE_DONE, stmt.execute());
  }
}

TEST(Sqlite3UtilsTest, CreateCustomVfsRealSyncLockAndOpenFailure) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  TempFile existing_db("sqlite3_utils_custom_real_sync_lock.db");
  TempFile missing_db("sqlite3_utils_custom_missing.db");
  CreateSampleDatabase(existing_db.path());

  auto vfs = NS_PROJ::SQLite3VFS::create(/*fakeSync=*/false, /*fakeLock=*/false,
                                         /*skipStatJournalAndWAL=*/false);
  ASSERT_THAT(vfs, NotNull());

  // 1. Real sync/lock VFS executes write transaction cleanly via default VFS.
  {
    sqlite3* raw_db = nullptr;
    const int open_rc = sqlite3_open_v2(existing_db.c_str(), &raw_db,
                                        SQLITE_OPEN_READWRITE, vfs->name());
    ScopedSqliteDb db(raw_db);
    ASSERT_EQ(SQLITE_OK, open_rc);
    EXPECT_EQ(SQLITE_OK,
              sqlite3_exec(db.get(),
                           "INSERT INTO items(id, label) VALUES(7, 'beta');",
                           nullptr, nullptr, nullptr));
  }

  // 2. Opening a nonexistent database read-only hits VFSCustomOpen error return
  // without wrapping file->pMethods.
  {
    sqlite3* raw_db = nullptr;
    const int open_rc = sqlite3_open_v2(missing_db.c_str(), &raw_db,
                                        SQLITE_OPEN_READONLY, vfs->name());
    ScopedSqliteDb db(raw_db);
    EXPECT_NE(SQLITE_OK, open_rc);
  }
}

TEST(Sqlite3UtilsTest, CreateCustomVfsSkipStatJournalAndWal) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  TempFile db_file("sqlite3_utils_skip_stat.db");
  CreateSampleDatabase(db_file.path());

  auto vfs = NS_PROJ::SQLite3VFS::create(/*fakeSync=*/false, /*fakeLock=*/true,
                                         /*skipStatJournalAndWAL=*/true);
  ASSERT_THAT(vfs, NotNull());

  // 1. Read-only open + query triggers -journal / -wal xAccess probes.
  {
    sqlite3* raw_db = nullptr;
    const int open_rc = sqlite3_open_v2(
        db_file.c_str(), &raw_db,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI,
        vfs->name());
    ScopedSqliteDb db(raw_db);
    ASSERT_EQ(SQLITE_OK, open_rc);
    ASSERT_THAT(db.get(), NotNull());

    sqlite3_stmt* raw_stmt = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                             db.get(), "SELECT label FROM items WHERE id=42",
                             -1, &raw_stmt, nullptr));
    NS_PROJ::SQLiteStatement stmt(raw_stmt);
    ASSERT_EQ(SQLITE_ROW, stmt.execute());
    EXPECT_STREQ("alpha", stmt.getText());
  }

  // 2. Explicitly exercise both "-journal" and "-wal" branches in
  // VFSCustomAccess().
  // Note: Never pass a path lacking "-journal" or "-wal" here because upstream
  // VFSCustomAccess casts vfs->pAppData (pj_sqlite3_customvfs_appdata*) to
  // sqlite3_vfs* before calling defaultVFS->xAccess.
  int exists = 1;
  EXPECT_EQ(SQLITE_OK, vfs->raw()->xAccess(vfs->raw(), "/mem/test.db-journal",
                                           SQLITE_ACCESS_EXISTS, &exists));
  EXPECT_EQ(0, exists);

  exists = 1;
  EXPECT_EQ(SQLITE_OK, vfs->raw()->xAccess(vfs->raw(), "/mem/test.db-wal",
                                           SQLITE_ACCESS_EXISTS, &exists));
  EXPECT_EQ(0, exists);
}

TEST(Sqlite3UtilsTest, CreateNetworkVfsQueriesDatabaseAndCoversIoMethods) {
  // Destroy file handles -> DB -> VFS -> context -> MockState -> proj_cleanup()
  // in exact reverse declaration order.
  absl::Cleanup cleanup = [] {
    NS_PROJ::FileManager::clearMemoryCache();
    proj_cleanup();
  };

  TempFile sample_db("sqlite3_utils_network_source.db");
  CreateSampleDatabase(sample_db.path());
  const std::vector<uint8_t> db_bytes = ReadFileBytes(sample_db.path());
  ASSERT_FALSE(db_bytes.empty());
  // Keep sample DB under DOWNLOAD_CHUNK_SIZE (16 KiB) so NetworkFile's initial
  // open fetch reads the entire file in chunk 0.
  ASSERT_LT(db_bytes.size(), size_t{16384});

  MockNetworkState mock_state{
      .bytes = db_bytes,
      .expected_url = "https://example.invalid/test.db",
      .content_range = "bytes 0-" + std::to_string(db_bytes.size() - 1) + "/" +
                       std::to_string(db_bytes.size()),
      .last_modified = "Wed, 21 Oct 2015 07:28:00 GMT",
      .etag = "\"etag-v1\"",
  };

  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  proj_grid_cache_set_enable(ctx.get(), 0);
  ASSERT_EQ(1, proj_context_set_network_callbacks(
                   ctx.get(), &MockNetworkState::Open, &MockNetworkState::Close,
                   &MockNetworkState::GetHeaderValue,
                   &MockNetworkState::ReadRange, &mock_state));

  auto vfs = NS_PROJ::SQLite3VFS::createNetwork(ctx.get());
  ASSERT_THAT(vfs, NotNull());
  ASSERT_THAT(vfs->name(), NotNull());

  // 1. Open and query the database over the network VFS through SQLite.
  {
    sqlite3* raw_db = nullptr;
    const int open_rc =
        sqlite3_open_v2(mock_state.expected_url.c_str(), &raw_db,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, vfs->name());
    ScopedSqliteDb db(raw_db);
    ASSERT_EQ(SQLITE_OK, open_rc);
    ASSERT_THAT(db.get(), NotNull());

    sqlite3_stmt* raw_stmt = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                             db.get(), "SELECT label FROM items WHERE id=42",
                             -1, &raw_stmt, nullptr));
    NS_PROJ::SQLiteStatement stmt(raw_stmt);
    ASSERT_EQ(SQLITE_ROW, stmt.execute());
    EXPECT_STREQ("alpha", stmt.getText());
    EXPECT_EQ(SQLITE_DONE, stmt.execute());
  }
  EXPECT_GT(mock_state.open_calls, 0);

  // Clear cached chunks so direct VFS calls test fresh open/error behavior.
  NS_PROJ::FileManager::clearMemoryCache();

  // 2. Exercise VFS pathname, access, delete, and open error branches directly.
  sqlite3_vfs* raw_vfs = vfs->raw();
  ASSERT_THAT(raw_vfs, NotNull());

  std::array<char, 64> exact_out{};
  // Equal length (no room for trailing NUL) must fail with SQLITE_CANTOPEN.
  EXPECT_EQ(
      SQLITE_CANTOPEN,
      raw_vfs->xFullPathname(raw_vfs, mock_state.expected_url.c_str(),
                             static_cast<int>(mock_state.expected_url.size()),
                             exact_out.data()));
  // Sufficient buffer succeeds and copies the URL verbatim.
  EXPECT_EQ(SQLITE_OK,
            raw_vfs->xFullPathname(raw_vfs, mock_state.expected_url.c_str(),
                                   static_cast<int>(exact_out.size()),
                                   exact_out.data()));
  EXPECT_STREQ(mock_state.expected_url.c_str(), exact_out.data());

  int access_res = 1;
  EXPECT_EQ(SQLITE_OK,
            raw_vfs->xAccess(raw_vfs, mock_state.expected_url.c_str(),
                             SQLITE_ACCESS_EXISTS, &access_res));
  EXPECT_EQ(0, access_res);
  EXPECT_EQ(SQLITE_IOERR_DELETE,
            raw_vfs->xDelete(raw_vfs, mock_state.expected_url.c_str(), 0));

  std::vector<std::max_align_t> file_storage(
      (raw_vfs->szOsFile + sizeof(std::max_align_t) - 1) /
      sizeof(std::max_align_t));
  auto* file = reinterpret_cast<sqlite3_file*>(file_storage.data());
  absl::Cleanup close_file = [&] {
    if (file->pMethods != nullptr && file->pMethods->xClose != nullptr) {
      file->pMethods->xClose(file);
      file->pMethods = nullptr;
    }
  };

  // Null filename fails cleanly.
  EXPECT_EQ(SQLITE_CANTOPEN, raw_vfs->xOpen(raw_vfs, nullptr, file,
                                            SQLITE_OPEN_READONLY, nullptr));

  // Missing remote resource logs an error and returns SQLITE_CANTOPEN.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_EQ(SQLITE_CANTOPEN,
              raw_vfs->xOpen(raw_vfs, "https://example.invalid/missing.db",
                             file, SQLITE_OPEN_READONLY, nullptr));
  }

  // 3. Open the valid network file directly to exercise all
  // VFSNetworkIOMethods.
  ASSERT_EQ(SQLITE_OK, raw_vfs->xOpen(raw_vfs, mock_state.expected_url.c_str(),
                                      file, SQLITE_OPEN_READONLY, nullptr));
  ASSERT_THAT(file->pMethods, NotNull());

  EXPECT_EQ(0, file->pMethods->xSectorSize(file));
  EXPECT_EQ(0, file->pMethods->xDeviceCharacteristics(file));
  int fcntl_dummy = 0;
  EXPECT_EQ(SQLITE_NOTFOUND, file->pMethods->xFileControl(
                                 file, SQLITE_FCNTL_LOCKSTATE, &fcntl_dummy));
  EXPECT_EQ(SQLITE_OK, file->pMethods->xSync(file, SQLITE_SYNC_NORMAL));
  EXPECT_EQ(SQLITE_OK, file->pMethods->xLock(file, SQLITE_LOCK_SHARED));
  EXPECT_EQ(SQLITE_OK, file->pMethods->xUnlock(file, SQLITE_LOCK_NONE));
  int reserved = 1;
  EXPECT_EQ(SQLITE_OK, file->pMethods->xCheckReservedLock(file, &reserved));
  EXPECT_EQ(0, reserved);
  EXPECT_EQ(SQLITE_IOERR_WRITE, file->pMethods->xWrite(file, "abc", 3, 0));
  EXPECT_EQ(SQLITE_IOERR_TRUNCATE, file->pMethods->xTruncate(file, 0));

  sqlite3_int64 file_size = -1;
  EXPECT_EQ(SQLITE_OK, file->pMethods->xFileSize(file, &file_size));
  EXPECT_EQ(static_cast<sqlite3_int64>(db_bytes.size()), file_size);

  // Full read of the SQLite file header ("SQLite format 3\0").
  std::array<char, 16> header{};
  EXPECT_EQ(SQLITE_OK, file->pMethods->xRead(file, header.data(), 16, 0));
  EXPECT_STREQ("SQLite format 3", header.data());

  // Short read across EOF must zero-fill unread trailing bytes and return
  // SQLITE_IOERR_SHORT_READ.
  std::array<uint8_t, 64> tail{};
  tail.fill(0xAA);
  const sqlite3_int64 tail_offset =
      static_cast<sqlite3_int64>(db_bytes.size()) - 8;
  EXPECT_EQ(SQLITE_IOERR_SHORT_READ,
            file->pMethods->xRead(file, tail.data(), 64, tail_offset));
  EXPECT_EQ(0, std::memcmp(db_bytes.data() + tail_offset, tail.data(), 8));
  for (size_t i = 8; i < tail.size(); ++i) {
    EXPECT_EQ(0, tail[i]) << "byte index " << i;
  }
}

// TODO(schwehr): Remaining uncovered lines in proj/src/sqlite3_utils.cpp:
//   - SQLite3VFS::createMem() (lines 432-468, compiled out because
//     EMBED_RESOURCE_FILES is unset in google3).
//   - InstallSqliteLogger PROJ_LOG_SQLITE3 env-var branch and
//     projSqlite3LogCallback() (lines 164-166, 172; process-global singleton +
//     sqlite3_config mutation).
//   - VFSCustomAccess non-journal/wal fallback (line 158; unsafe because
//     upstream casts pj_sqlite3_customvfs_appdata* to sqlite3_vfs*).
//   - VFSNetworkRead seek-failure return SQLITE_IOERR_READ (line 254;
//     unreachable with NetworkFile::seek(SEEK_SET)), sqlite3_vfs_register()
//     failure paths (lines 231-233, 427), and VFSCustomOpen malloc OOM
//     (125-126).

}  // namespace
}  // namespace proj
