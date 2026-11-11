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
// Unit tests for PROJ network access and SQLite chunk/download caching
// (proj/src/networkfilemanager.cpp). In google3 builds,
// proj_context_is_network_enabled() is patched to return FALSE (0) by default;
// //third_party/proj4:proj4_unbundled marks it weak via
// //third_party/proj4/google/util:weak_network_enabled.h so this test can
// supply a strong in-process override when exercising network callback logic.
// All network requests in this file are served in-process by a mock callback.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/networkfilemanager.cpp

#include <algorithm>
#include <atomic>
#include <chrono>              // NOLINT(build/c++11)
#include <condition_variable>  // NOLINT(build/c++11)
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>  // NOLINT(build/c++11)
#include <thread>        // NOLINT
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/google/util/temp_file.h"
#include "third_party/proj4/proj/src/filemanager.hpp"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"
#include "third_party/sqlite/sqlite3.h"

namespace {
std::atomic<bool> g_use_context_network_flag{false};
}  // namespace

extern "C" {
int proj_context_is_network_enabled(PJ_CONTEXT* ctx) {
  if (!g_use_context_network_flag.load(std::memory_order_relaxed)) {
    return 0;
  }
  PJ_CONTEXT* target = ctx != nullptr ? ctx : pj_get_default_ctx();
  return target->networking.enabled ? 1 : 0;
}
}  // extern "C"

namespace proj {
namespace {

using ::testing::IsEmpty;
using ::testing::NotNull;

constexpr size_t kChunkSize = 16 * 1024;
constexpr double kDegToRad = std::numbers::pi / 180.0;

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

class ScopedEnvVar {
 public:
  explicit ScopedEnvVar(const char* name) : name_(name) {
    if (const char* existing = std::getenv(name_)) {
      original_ = existing;
    }
  }

  ScopedEnvVar(const char* name, const char* value) : ScopedEnvVar(name) {
    Set(value);
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

struct SqliteDbCloser {
  void operator()(sqlite3* db) const noexcept {
    if (db != nullptr) {
      sqlite3_close(db);
    }
  }
};

using ScopedSqliteDb = std::unique_ptr<sqlite3, SqliteDbCloser>;

ScopedSqliteDb OpenSqliteDb(const char* path) {
  sqlite3* raw = nullptr;
  if (sqlite3_open_v2(path, &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      nullptr) != SQLITE_OK) {
    sqlite3_close(raw);
    return nullptr;
  }
  return ScopedSqliteDb(raw);
}

struct SqliteStmtFinalizer {
  void operator()(sqlite3_stmt* stmt) const noexcept {
    if (stmt != nullptr) {
      sqlite3_finalize(stmt);
    }
  }
};

using ScopedSqliteStmt = std::unique_ptr<sqlite3_stmt, SqliteStmtFinalizer>;

bool ExecSql(sqlite3* db, const char* sql) {
  return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::vector<uint8_t> MakePatternData(size_t size, uint8_t seed = 0) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>((i * 31 + (i >> 8) + seed) & 0xFF);
  }
  return data;
}

class MockNetworkServer {
 public:
  struct RemoteEntry {
    std::vector<uint8_t> data;
    std::string etag;
    std::string last_modified;
    bool omit_content_range = false;
    bool bad_content_range = false;
    bool fail_open = false;
    std::string open_error = "mock open failure";
    bool fail_read_range = false;
    std::string read_error = "mock read failure";
    bool force_zero_open_read = false;
    size_t max_read_range_bytes = 0;
    std::function<void(RemoteEntry&)> on_read_range;
  };

  void AddFile(std::string url, RemoteEntry entry) {
    std::lock_guard<std::mutex> lock(mu_);  // NOLINT(build/c++11)
    files_[std::move(url)] = std::move(entry);
  }

  void AddBytes(std::string url, std::vector<uint8_t> data,
                std::string etag = "\"v1\"",
                std::string last_modified = "Mon, 01 Jan 2026 00:00:00 GMT") {
    AddFile(std::move(url),
            RemoteEntry{.data = std::move(data),
                        .etag = std::move(etag),
                        .last_modified = std::move(last_modified)});
  }

  bool Install(PJ_CONTEXT* ctx, bool enable = true) {
    if (proj_context_set_network_callbacks(ctx, &OpenCbk, &CloseCbk,
                                           &GetHeaderValueCbk, &ReadRangeCbk,
                                           this) == 0) {
      return false;
    }
    if (enable) {
      return proj_context_set_enable_network(ctx, 1) != 0;
    }
    return true;
  }

  int open_count() const { return open_count_.load(std::memory_order_relaxed); }
  int close_count() const {
    return close_count_.load(std::memory_order_relaxed);
  }
  int read_range_count() const {
    return read_range_count_.load(std::memory_order_relaxed);
  }
  void ResetCounts() {
    open_count_.store(0, std::memory_order_relaxed);
    close_count_.store(0, std::memory_order_relaxed);
    read_range_count_.store(0, std::memory_order_relaxed);
  }

 private:
  struct Handle {
    MockNetworkServer* server;
    std::string url;
    std::string content_range;
    std::string etag;
    std::string last_modified;
    bool omit_content_range = false;
    bool bad_content_range = false;
  };

  static void PopulateHandleHeaders(Handle* h, const RemoteEntry& entry,
                                    // NOLINTNEXTLINE(runtime/int)
                                    unsigned long long offset, size_t n_read) {
    const unsigned long long total = entry.data.size();  // NOLINT(runtime/int)
    if (total == 0) {
      h->content_range = "bytes */0";
    } else {
      // NOLINTNEXTLINE(runtime/int)
      const unsigned long long end = n_read > 0 ? offset + n_read - 1 : offset;
      h->content_range = "bytes " + std::to_string(offset) + "-" +
                         std::to_string(end) + "/" + std::to_string(total);
    }
    h->etag = entry.etag;
    h->last_modified = entry.last_modified;
    h->omit_content_range = entry.omit_content_range;
    h->bad_content_range = entry.bad_content_range;
  }

  static void WriteErr(size_t max_size, char* buf, const std::string& msg) {
    if (buf != nullptr && max_size > 0) {
      std::snprintf(buf, max_size, "%s", msg.c_str());
    }
  }

  static PROJ_NETWORK_HANDLE* OpenCbk(PJ_CONTEXT* /*ctx*/, const char* url,
                                      // NOLINTNEXTLINE(runtime/int)
                                      unsigned long long offset,
                                      size_t size_to_read, void* buffer,
                                      size_t* out_size_read,
                                      size_t error_string_max_size,
                                      char* out_error_string, void* user_data) {
    auto* self = static_cast<MockNetworkServer*>(user_data);
    self->open_count_.fetch_add(1, std::memory_order_relaxed);
    if (url == nullptr || self == nullptr) {
      WriteErr(error_string_max_size, out_error_string, "null url");
      return nullptr;
    }

    std::lock_guard<std::mutex> lock(self->mu_);  // NOLINT(build/c++11)
    auto it = self->files_.find(url);
    if (it == self->files_.end()) {
      WriteErr(error_string_max_size, out_error_string, "404 Not Found");
      return nullptr;
    }
    const RemoteEntry& entry = it->second;
    if (entry.fail_open) {
      WriteErr(error_string_max_size, out_error_string, entry.open_error);
      return nullptr;
    }

    size_t n_read = 0;
    if (!entry.force_zero_open_read && offset < entry.data.size()) {
      n_read = std::min(size_to_read,
                        static_cast<size_t>(entry.data.size() - offset));
      if (n_read > 0 && buffer != nullptr) {
        std::memcpy(buffer, entry.data.data() + offset, n_read);
      }
    }
    if (out_size_read != nullptr) {
      *out_size_read = n_read;
    }

    auto* h = new Handle{.server = self, .url = url};
    PopulateHandleHeaders(h, entry, offset, n_read);
    return reinterpret_cast<PROJ_NETWORK_HANDLE*>(h);
  }

  static void CloseCbk(PJ_CONTEXT* /*ctx*/, PROJ_NETWORK_HANDLE* raw_handle,
                       void* user_data) {
    auto* self = static_cast<MockNetworkServer*>(user_data);
    if (self != nullptr) {
      self->close_count_.fetch_add(1, std::memory_order_relaxed);
    }
    delete reinterpret_cast<Handle*>(raw_handle);
  }

  static const char* GetHeaderValueCbk(PJ_CONTEXT* /*ctx*/,
                                       PROJ_NETWORK_HANDLE* raw_handle,
                                       const char* header_name,
                                       void* /*user_data*/) {
    if (raw_handle == nullptr || header_name == nullptr) {
      return nullptr;
    }
    auto* h = reinterpret_cast<Handle*>(raw_handle);
    if (std::strcmp(header_name, "Content-Range") == 0) {
      if (h->omit_content_range) {
        return nullptr;
      }
      if (h->bad_content_range) {
        return "bytes 0-10";
      }
      return h->content_range.c_str();
    }
    if (std::strcmp(header_name, "Last-Modified") == 0) {
      return h->last_modified.empty() ? nullptr : h->last_modified.c_str();
    }
    if (std::strcmp(header_name, "ETag") == 0) {
      return h->etag.empty() ? nullptr : h->etag.c_str();
    }
    return nullptr;
  }

  static size_t ReadRangeCbk(PJ_CONTEXT* /*ctx*/,
                             PROJ_NETWORK_HANDLE* raw_handle,
                             // NOLINTNEXTLINE(runtime/int)
                             unsigned long long offset, size_t size_to_read,
                             void* buffer, size_t error_string_max_size,
                             char* out_error_string, void* /*user_data*/) {
    if (raw_handle == nullptr) {
      return 0;
    }
    auto* h = reinterpret_cast<Handle*>(raw_handle);
    MockNetworkServer* self = h->server;
    self->read_range_count_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(self->mu_);  // NOLINT(build/c++11)
    auto it = self->files_.find(h->url);
    if (it == self->files_.end()) {
      WriteErr(error_string_max_size, out_error_string, "missing entry");
      return 0;
    }
    RemoteEntry& entry = it->second;
    if (entry.on_read_range) {
      entry.on_read_range(entry);
    }
    if (entry.fail_read_range) {
      WriteErr(error_string_max_size, out_error_string, entry.read_error);
      return 0;
    }
    if (offset >= entry.data.size()) {
      PopulateHandleHeaders(h, entry, offset, 0);
      return 0;
    }
    size_t n_read =
        std::min(size_to_read, static_cast<size_t>(entry.data.size() - offset));
    if (entry.max_read_range_bytes > 0) {
      n_read = std::min(n_read, entry.max_read_range_bytes);
    }
    if (n_read > 0 && buffer != nullptr) {
      std::memcpy(buffer, entry.data.data() + offset, n_read);
    }
    PopulateHandleHeaders(h, entry, offset, n_read);
    return n_read;
  }

  mutable std::mutex mu_;  // NOLINT(build/c++11)
  std::map<std::string, RemoteEntry, std::less<>> files_;
  std::atomic<int> open_count_{0};
  std::atomic<int> close_count_{0};
  std::atomic<int> read_range_count_{0};
};

// In-memory PROJ_FILE_API supporting fault injection for create, write, and
// rename calls inside proj_download_file() error tests.
class FaultyMemoryFileSystem {
 public:
  bool fail_create = false;
  int fail_write_on_call = 0;  // 1-based write invocation to fail.
  bool fail_rename = false;

  void ResetFaults() {
    fail_create = false;
    fail_write_on_call = 0;
    fail_rename = false;
    write_call_count_ = 0;
  }

  bool Install(PJ_CONTEXT* ctx) {
    return proj_context_set_fileapi(ctx, &kApi, this) != 0;
  }

  bool Contains(std::string_view path) const {
    return files_.find(path) != files_.end();
  }

 private:
  struct Handle {
    FaultyMemoryFileSystem* fs;
    std::string path;
    PROJ_OPEN_ACCESS access;
    size_t pos = 0;
  };

  static PROJ_FILE_HANDLE* OpenCbk(PJ_CONTEXT*, const char* filename,
                                   PROJ_OPEN_ACCESS access, void* user_data) {
    if (filename == nullptr || user_data == nullptr) {
      return nullptr;
    }
    auto* fs = static_cast<FaultyMemoryFileSystem*>(user_data);
    if (access == PROJ_OPEN_ACCESS_READ_ONLY) {
      if (fs->files_.find(filename) == fs->files_.end()) {
        return nullptr;
      }
    } else if (access == PROJ_OPEN_ACCESS_READ_UPDATE) {
      fs->files_.try_emplace(filename);
    } else if (access == PROJ_OPEN_ACCESS_CREATE) {
      if (fs->fail_create) {
        return nullptr;
      }
      fs->files_[filename].clear();
    } else {
      return nullptr;
    }
    return reinterpret_cast<PROJ_FILE_HANDLE*>(
        new Handle{.fs = fs, .path = filename, .access = access, .pos = 0});
  }

  static size_t ReadCbk(PJ_CONTEXT*, PROJ_FILE_HANDLE* file, void* buffer,
                        size_t size_bytes, void*) {
    auto* h = reinterpret_cast<Handle*>(file);
    if (h == nullptr || buffer == nullptr || size_bytes == 0) {
      return 0;
    }
    const auto it = h->fs->files_.find(h->path);
    if (it == h->fs->files_.end() || h->pos >= it->second.size()) {
      return 0;
    }
    const size_t n = std::min(size_bytes, it->second.size() - h->pos);
    std::memcpy(buffer, it->second.data() + h->pos, n);
    h->pos += n;
    return n;
  }

  static size_t WriteCbk(PJ_CONTEXT*, PROJ_FILE_HANDLE* file,
                         const void* buffer, size_t size_bytes, void*) {
    auto* h = reinterpret_cast<Handle*>(file);
    if (h == nullptr || (buffer == nullptr && size_bytes > 0) ||
        h->access == PROJ_OPEN_ACCESS_READ_ONLY) {
      return 0;
    }
    ++h->fs->write_call_count_;
    if (h->fs->fail_write_on_call > 0 &&
        h->fs->write_call_count_ == h->fs->fail_write_on_call) {
      return 0;
    }
    auto it = h->fs->files_.find(h->path);
    if (it == h->fs->files_.end() || size_bytes == 0) {
      return 0;
    }
    if (h->pos + size_bytes > it->second.size()) {
      it->second.resize(h->pos + size_bytes, 0);
    }
    std::memcpy(it->second.data() + h->pos, buffer, size_bytes);
    h->pos += size_bytes;
    return size_bytes;
  }

  // NOLINTNEXTLINE(runtime/int)
  static int SeekCbk(PJ_CONTEXT*, PROJ_FILE_HANDLE* file, long long offset,
                     int whence, void*) {
    auto* h = reinterpret_cast<Handle*>(file);
    if (h == nullptr) {
      return 0;
    }
    const auto it = h->fs->files_.find(h->path);
    if (it == h->fs->files_.end()) {
      return 0;
    }
    long long base = 0;  // NOLINT(runtime/int)
    if (whence == SEEK_CUR) {
      base = static_cast<long long>(h->pos);
    } else if (whence == SEEK_END) {
      base = static_cast<long long>(it->second.size());
    } else if (whence != SEEK_SET) {
      return 0;
    }
    if (base + offset < 0) {
      return 0;
    }
    h->pos = static_cast<size_t>(base + offset);
    return 1;
  }

  // NOLINTNEXTLINE(runtime/int)
  static unsigned long long TellCbk(PJ_CONTEXT*, PROJ_FILE_HANDLE* file,
                                    void*) {
    return file != nullptr ? reinterpret_cast<Handle*>(file)->pos : 0;
  }

  static void CloseCbk(PJ_CONTEXT*, PROJ_FILE_HANDLE* file, void*) {
    delete reinterpret_cast<Handle*>(file);
  }

  static int ExistsCbk(PJ_CONTEXT*, const char* filename, void* user_data) {
    auto* fs = static_cast<FaultyMemoryFileSystem*>(user_data);
    if (filename == nullptr || fs == nullptr) {
      return 0;
    }
    return (fs->files_.find(filename) != fs->files_.end() ||
            fs->dirs_.find(filename) != fs->dirs_.end())
               ? 1
               : 0;
  }

  static int MkdirCbk(PJ_CONTEXT*, const char* filename, void* user_data) {
    auto* fs = static_cast<FaultyMemoryFileSystem*>(user_data);
    if (filename == nullptr || fs == nullptr) {
      return 0;
    }
    fs->dirs_.insert(filename);
    return 1;
  }

  static int UnlinkCbk(PJ_CONTEXT*, const char* filename, void* user_data) {
    auto* fs = static_cast<FaultyMemoryFileSystem*>(user_data);
    if (filename == nullptr || fs == nullptr) {
      return 0;
    }
    return fs->files_.erase(filename) > 0 ? 1 : 0;
  }

  static int RenameCbk(PJ_CONTEXT*, const char* old_path, const char* new_path,
                       void* user_data) {
    auto* fs = static_cast<FaultyMemoryFileSystem*>(user_data);
    if (old_path == nullptr || new_path == nullptr || fs == nullptr ||
        fs->fail_rename) {
      return 0;
    }
    auto it = fs->files_.find(old_path);
    if (it == fs->files_.end()) {
      return 0;
    }
    if (std::strcmp(old_path, new_path) != 0) {
      fs->files_[new_path] = std::move(it->second);
      fs->files_.erase(it);
    }
    return 1;
  }

  static constexpr PROJ_FILE_API kApi = {
      /*version=*/1, &OpenCbk,   &ReadCbk,   &WriteCbk,
      &SeekCbk,      &TellCbk,   &CloseCbk,  &ExistsCbk,
      &MkdirCbk,     &UnlinkCbk, &RenameCbk,
  };

  int write_call_count_ = 0;
  std::map<std::string, std::vector<uint8_t>, std::less<>> files_;
  std::set<std::string, std::less<>> dirs_;
};

void AltCloseCallback(PJ_CONTEXT* /*ctx*/, PROJ_NETWORK_HANDLE* /*handle*/,
                      void* /*user_data*/) {}

class NetworkFileManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    proj_cleanup();
    g_use_context_network_flag.store(false, std::memory_order_relaxed);
  }

  void TearDown() override {
    g_use_context_network_flag.store(false, std::memory_order_relaxed);
    proj_cleanup();
  }
};

TEST_F(NetworkFileManagerTest,
       PatchedDefaultDisablesNetworkAndCallbacksValidation) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);

  // Google3 patch guarantees proj_context_is_network_enabled returns FALSE by
  // default when the test wrapper is inactive.
  EXPECT_EQ(0, proj_context_is_network_enabled(ctx.get()));

  // Default no-op interface rejects enablement and reports unavailable.
  EXPECT_EQ(0, proj_context_set_enable_network(ctx.get(), 1));
  EXPECT_EQ(nullptr, ::NS_PROJ::pj_network_file_open(
                         ctx.get(), "https://example.invalid/grid.gtx")
                         .get());
  EXPECT_EQ(PROJ_ERR_OTHER_NETWORK_ERROR, proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("Network functionality not available"));
  proj_context_errno_set(ctx.get(), 0);

  // Direct call to no_op_network_open with nullptr error buffer is safe.
  size_t dummy_read = 99;
  uint8_t dummy_buf[4] = {};
  EXPECT_EQ(nullptr,
            ctx->networking.open(ctx.get(), "https://example.invalid/x", 0, 4,
                                 dummy_buf, &dummy_read, 0, nullptr, nullptr));
  ctx->networking.close(ctx.get(), nullptr, nullptr);

  MockNetworkServer server;
  // Reject any missing callback pointer.
  EXPECT_EQ(0, proj_context_set_network_callbacks(ctx.get(), nullptr,
                                                  ctx->networking.close,
                                                  nullptr, nullptr, nullptr));
  // Install valid callbacks (first test extracting via server.Install).
  ASSERT_TRUE(server.Install(ctx.get(), /*enable=*/false));
  // Missing individual callbacks fails cleanly.
  EXPECT_EQ(0, proj_context_set_network_callbacks(
                   ctx.get(), ctx->networking.open, nullptr,
                   ctx->networking.get_header_value, ctx->networking.read_range,
                   nullptr));
  EXPECT_EQ(0, proj_context_set_network_callbacks(
                   ctx.get(), ctx->networking.open, ctx->networking.close,
                   nullptr, ctx->networking.read_range, nullptr));
  EXPECT_EQ(0, proj_context_set_network_callbacks(
                   ctx.get(), ctx->networking.open, ctx->networking.close,
                   ctx->networking.get_header_value, nullptr, nullptr));

  // Enabling networking succeeds with custom callbacks, yet wrapper stays FALSE
  // while ScopedNetworkWrapper is inactive.
  EXPECT_EQ(1, proj_context_set_enable_network(ctx.get(), 1));
  EXPECT_EQ(0, proj_context_is_network_enabled(ctx.get()));

  // Enabling test wrapper allows context flag to take effect.
  {
    ScopedNetworkWrapper wrapper(true);
    EXPECT_EQ(1, proj_context_is_network_enabled(ctx.get()));
    EXPECT_EQ(0, proj_context_set_enable_network(ctx.get(), 0));
    EXPECT_EQ(0, proj_context_is_network_enabled(ctx.get()));
  }
  EXPECT_EQ(0, proj_context_is_network_enabled(ctx.get()));

  // Resetting to default network interface restores no-op behavior.
  ::NS_PROJ::FileManager::fillDefaultNetworkInterface(ctx.get());
  EXPECT_EQ(0, proj_context_set_enable_network(ctx.get(), 1));
}

TEST_F(NetworkFileManagerTest, DefaultContextNullptrFallbacksAndRestore) {
  PJ_CONTEXT* default_ctx = pj_get_default_ctx();
  ASSERT_THAT(default_ctx, NotNull());

  const auto saved_networking = default_ctx->networking;
  const std::string saved_endpoint = default_ctx->endpoint;
  const auto saved_cache = default_ctx->gridChunkCache;
  const bool saved_ini_loaded = default_ctx->iniFileLoaded;
  const std::string saved_writable_dir = default_ctx->user_writable_directory;
  const int saved_debug_level = default_ctx->debug_level;
  auto* const saved_logger = default_ctx->logger;
  void* const saved_logger_data = default_ctx->logger_app_data;

  absl::Cleanup restore_default_ctx = [&] {
    default_ctx->networking = saved_networking;
    default_ctx->endpoint = saved_endpoint;
    default_ctx->gridChunkCache = saved_cache;
    default_ctx->iniFileLoaded = saved_ini_loaded;
    default_ctx->user_writable_directory = saved_writable_dir;
    default_ctx->debug_level = saved_debug_level;
    default_ctx->logger = saved_logger;
    default_ctx->logger_app_data = saved_logger_data;
    proj_context_errno_set(default_ctx, 0);
  };

  ScopedLogSuppressor silence_default(default_ctx);
  TempDir dir("nfm_default_ctx");
  proj_context_set_user_writable_directory(nullptr, dir.c_str(), 1);
  proj_grid_cache_set_enable(nullptr, 1);
  proj_grid_cache_set_filename(nullptr, dir.child("default_cache.db").c_str());
  proj_grid_cache_set_max_size(nullptr, 1);
  proj_grid_cache_set_ttl(nullptr, 60);
  proj_context_set_url_endpoint(nullptr, "");

  MockNetworkServer server;
  server.AddBytes("https://mock.invalid/null_ctx.bin", MakePatternData(128));
  ASSERT_TRUE(server.Install(nullptr, /*enable=*/true));

  ScopedNetworkWrapper wrapper(true);
  EXPECT_EQ(1, proj_context_is_network_enabled(nullptr));

  // Empty endpoint with bare filename -> build_url("") -> strrchr == nullptr.
  EXPECT_EQ(0, proj_is_download_needed(nullptr, "bare.bin", 0));
  EXPECT_EQ(1, proj_download_file(nullptr, "bare.bin", 0, nullptr, nullptr));

  // Full URL triggers real download + cache write via default ctx.
  EXPECT_EQ(1, proj_is_download_needed(nullptr,
                                       "https://mock.invalid/null_ctx.bin", 0));
  EXPECT_EQ(1, proj_download_file(nullptr, "https://mock.invalid/null_ctx.bin",
                                  0, nullptr, nullptr));
  EXPECT_EQ(0, proj_is_download_needed(nullptr,
                                       "https://mock.invalid/null_ctx.bin", 0));
  proj_grid_cache_clear(nullptr);
}

TEST_F(NetworkFileManagerTest, GridCacheSettingsAndFilename) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  TempDir dir("nfm_cache_cfg");
  proj_context_set_user_writable_directory(ctx.get(), dir.c_str(), 1);

  // Clearing with cache disabled is a safe no-op.
  proj_grid_cache_set_enable(ctx.get(), 0);
  EXPECT_FALSE(ctx->gridChunkCache.enabled);
  proj_grid_cache_clear(ctx.get());

  // Clearing with filename = nullptr clears the configured path; querying
  // pj_context_get_grid_cache_filename() recomputes user_writable_dir/cache.db.
  proj_grid_cache_set_enable(ctx.get(), 1);
  proj_grid_cache_set_filename(ctx.get(), nullptr);
  EXPECT_THAT(ctx->gridChunkCache.filename, IsEmpty());
  EXPECT_EQ(dir.child("cache.db").string(),
            pj_context_get_grid_cache_filename(ctx.get()));
  EXPECT_EQ(dir.child("cache.db").string(), ctx->gridChunkCache.filename);

  // Passing an explicit filename overrides default.
  const std::string custom_db = dir.child("custom_cache.db").string();
  proj_grid_cache_set_filename(ctx.get(), custom_db.c_str());
  EXPECT_EQ(custom_db, pj_context_get_grid_cache_filename(ctx.get()));

  // Max size: negative (-1), positive (MB -> bytes), 0 with debug env var.
  proj_grid_cache_set_max_size(ctx.get(), -1);
  EXPECT_EQ(-1, ctx->gridChunkCache.max_size);
  proj_grid_cache_set_max_size(ctx.get(), 2);
  EXPECT_EQ(2LL * 1024 * 1024, ctx->gridChunkCache.max_size);
  {
    ScopedEnvVar max_bytes("PROJ_GRID_CACHE_MAX_SIZE_BYTES", "32768");
    proj_grid_cache_set_max_size(ctx.get(), 0);
    EXPECT_EQ(32768, ctx->gridChunkCache.max_size);
    max_bytes.Set("");
    proj_grid_cache_set_max_size(ctx.get(), 0);
    EXPECT_EQ(0, ctx->gridChunkCache.max_size);
  }
  proj_grid_cache_set_max_size(ctx.get(), 0);
  EXPECT_EQ(0, ctx->gridChunkCache.max_size);

  proj_grid_cache_set_ttl(ctx.get(), 123);
  EXPECT_EQ(123, ctx->gridChunkCache.ttl);

  proj_grid_cache_clear(ctx.get());
  EXPECT_FALSE(std::filesystem::exists(custom_db));
}

TEST_F(NetworkFileManagerTest, NetworkFileOpenErrorsSeekAndReadHeuristics) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
  ScopedNetworkWrapper wrapper(true);
  TempDir dir("nfm_file_io");
  proj_context_set_user_writable_directory(ctx.get(), dir.c_str(), 1);
  proj_grid_cache_set_enable(ctx.get(), 0);

  MockNetworkServer server;
  ASSERT_TRUE(server.Install(ctx.get()));

  // 1. Missing URL (open returns nullptr with errorBuffer).
  EXPECT_EQ(nullptr, ::NS_PROJ::pj_network_file_open(
                         ctx.get(), "https://mock.invalid/missing.bin")
                         .get());
  EXPECT_EQ(PROJ_ERR_OTHER_NETWORK_ERROR, proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("404 Not Found"));
  proj_context_errno_set(ctx.get(), 0);

  // 2. Missing Content-Range header -> open closes handle and returns nullptr.
  server.AddFile("https://mock.invalid/no_cr.bin",
                 {.data = MakePatternData(64), .omit_content_range = true});
  EXPECT_EQ(nullptr, ::NS_PROJ::pj_network_file_open(
                         ctx.get(), "https://mock.invalid/no_cr.bin")
                         .get());
  EXPECT_EQ(1, server.close_count());

  // 3. Content-Range without '/' -> open closes handle and returns nullptr.
  server.AddFile("https://mock.invalid/bad_cr.bin",
                 {.data = MakePatternData(64), .bad_content_range = true});
  EXPECT_EQ(nullptr, ::NS_PROJ::pj_network_file_open(
                         ctx.get(), "https://mock.invalid/bad_cr.bin")
                         .get());
  EXPECT_EQ(2, server.close_count());

  // 4. Sequential doubling up to MAX_CHUNKS (64) + min-blocks + seek/write.
  const std::vector<uint8_t> big_data =
      MakePatternData(135 * kChunkSize, /*seed=*/3);
  server.AddFile("https://mock.invalid/big.bin",
                 {.data = big_data, .etag = "", .last_modified = ""});

  auto file = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                              "https://mock.invalid/big.bin");
  ASSERT_THAT(file.get(), NotNull());
  EXPECT_FALSE(file->hasChanged());
  EXPECT_EQ(0u, file->write(big_data.data(), 4));
  std::vector<uint8_t> out(65 * kChunkSize);
  EXPECT_EQ(0u, file->read(out.data(), 0));

  // Verify seek modes including SEEK_END failure when offset != 0.
  EXPECT_FALSE(file->seek(1, SEEK_END));
  EXPECT_TRUE(file->seek(0, SEEK_END));
  EXPECT_EQ(big_data.size(), file->tell());
  EXPECT_TRUE(file->seek(kChunkSize, SEEK_SET));
  EXPECT_EQ(kChunkSize, file->tell());
  EXPECT_TRUE(file->seek(0, SEEK_CUR));
  EXPECT_EQ(kChunkSize, file->tell());

  // Sequential single-byte reads at chunk boundaries step prefetch size:
  // chunk 1 -> 2 blocks (1..2), chunk 3 -> 4 (3..6), chunk 7 -> 8 (7..14),
  // chunk 15 -> 16 (15..30), chunk 31 -> 32 (31..62), chunk 63 -> 64 (63..126).
  uint8_t byte = 0;
  for (size_t chunk_idx : {1, 3, 7, 15, 31, 63}) {
    ASSERT_TRUE(file->seek(chunk_idx * kChunkSize, SEEK_SET));
    ASSERT_EQ(1u, file->read(&byte, 1));
    EXPECT_EQ(big_data[chunk_idx * kChunkSize], byte);
  }
  // At m_nBlocksToDownload == 64 and m_lastDownloadedOffset == 127*kChunkSize,
  // reading chunk 127 doubles 64 -> 128 and caps to MAX_CHUNKS (64).
  ASSERT_TRUE(file->seek(127 * kChunkSize, SEEK_SET));
  ASSERT_EQ(1u, file->read(&byte, 1));
  EXPECT_EQ(big_data[127 * kChunkSize], byte);
  file.reset();

  // Verify large read triggering nMinBlocksToDownload > MAX_CHUNKS and loop.
  ::NS_PROJ::FileManager::clearMemoryCache();
  file = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                         "https://mock.invalid/big.bin");
  ASSERT_THAT(file.get(), NotNull());
  ASSERT_TRUE(file->seek(kChunkSize, SEEK_SET));
  ASSERT_EQ(out.size(), file->read(out.data(), out.size()));
  EXPECT_EQ(0,
            std::memcmp(out.data(), big_data.data() + kChunkSize, out.size()));
  file.reset();

  // 5. Stop-before-cached-chunk + random-read reset heuristic.
  ::NS_PROJ::FileManager::clearMemoryCache();
  const std::vector<uint8_t> six_chunks =
      MakePatternData(6 * kChunkSize, /*seed=*/7);
  server.AddBytes("https://mock.invalid/six.bin", six_chunks);
  file = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                         "https://mock.invalid/six.bin");
  ASSERT_THAT(file.get(), NotNull());
  // Cache chunk 4 via random read.
  ASSERT_TRUE(file->seek(4 * kChunkSize, SEEK_SET));
  ASSERT_EQ(1u, file->read(&byte, 1));
  // Read chunks 1..5 in one call -> stops before chunk 4, then uses cached 4,
  // then fetches chunk 5.
  std::vector<uint8_t> five_out(5 * kChunkSize);
  ASSERT_TRUE(file->seek(kChunkSize, SEEK_SET));
  ASSERT_EQ(five_out.size(), file->read(five_out.data(), five_out.size()));
  EXPECT_EQ(0, std::memcmp(five_out.data(), six_chunks.data() + kChunkSize,
                           five_out.size()));
  file.reset();

  // 6. Partial last chunk (EOF smaller than DOWNLOAD_CHUNK_SIZE).
  const std::vector<uint8_t> short_tail =
      MakePatternData(kChunkSize + 512, /*seed=*/9);
  server.AddBytes("https://mock.invalid/tail.bin", short_tail);
  file = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                         "https://mock.invalid/tail.bin");
  ASSERT_THAT(file.get(), NotNull());
  std::vector<uint8_t> tail_buf(2048);
  ASSERT_TRUE(file->seek(kChunkSize, SEEK_SET));
  EXPECT_EQ(512u, file->read(tail_buf.data(), tail_buf.size()));
  EXPECT_EQ(0,
            std::memcmp(tail_buf.data(), short_tail.data() + kChunkSize, 512));
  file.reset();

  // 7. read_range errors (with non-empty and empty error messages).
  server.AddFile("https://mock.invalid/read_err.bin",
                 {.data = MakePatternData(2 * kChunkSize),
                  .etag = "\"r1\"",
                  .fail_read_range = true,
                  .read_error = "range boom"});
  file = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                         "https://mock.invalid/read_err.bin");
  ASSERT_THAT(file.get(), NotNull());
  ASSERT_TRUE(file->seek(kChunkSize, SEEK_SET));
  logs.Clear();
  EXPECT_EQ(0u, file->read(tail_buf.data(), 16));
  EXPECT_EQ(PROJ_ERR_OTHER_NETWORK_ERROR, proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("range boom"));
  proj_context_errno_set(ctx.get(), 0);
  file.reset();

  server.AddFile("https://mock.invalid/read_err_empty.bin",
                 {.data = MakePatternData(2 * kChunkSize),
                  .etag = "\"r2\"",
                  .fail_read_range = true,
                  .read_error = ""});
  file = ::NS_PROJ::pj_network_file_open(
      ctx.get(), "https://mock.invalid/read_err_empty.bin");
  ASSERT_THAT(file.get(), NotNull());
  ASSERT_TRUE(file->seek(kChunkSize, SEEK_SET));
  EXPECT_EQ(0u, file->read(tail_buf.data(), 16));
  EXPECT_EQ(PROJ_ERR_OTHER_NETWORK_ERROR, proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);
  file.reset();

  // 8. Mid-read server change detection marks hasChanged() == true.
  server.AddFile("https://mock.invalid/changing.bin",
                 {.data = MakePatternData(3 * kChunkSize),
                  .etag = "\"old\"",
                  .on_read_range = [](MockNetworkServer::RemoteEntry& e) {
                    e.etag = "\"new\"";
                  }});
  file = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                         "https://mock.invalid/changing.bin");
  ASSERT_THAT(file.get(), NotNull());
  EXPECT_FALSE(file->hasChanged());
  ASSERT_TRUE(file->seek(kChunkSize, SEEK_SET));
  EXPECT_EQ(16u, file->read(tail_buf.data(), 16));
  EXPECT_TRUE(file->hasChanged());
  // Subsequent uncached read skips header re-comparison when already changed.
  ASSERT_TRUE(file->seek(2 * kChunkSize, SEEK_SET));
  EXPECT_EQ(16u, file->read(tail_buf.data(), 16));
  EXPECT_TRUE(file->hasChanged());
}

TEST_F(NetworkFileManagerTest, LazyOpenOnCacheHitAndReassignContextMismatch) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
  ScopedNetworkWrapper wrapper(true);
  TempDir dir("nfm_lazy_open");
  proj_context_set_user_writable_directory(ctx.get(), dir.c_str(), 1);
  proj_grid_cache_set_filename(ctx.get(), dir.child("cache.db").c_str());

  const std::vector<uint8_t> two_chunks = MakePatternData(kChunkSize + 200);
  MockNetworkServer server;
  server.AddBytes("https://mock.invalid/two.bin", two_chunks);
  ASSERT_TRUE(server.Install(ctx.get()));

  // Populate chunk 0 only into memory/disk cache.
  {
    auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                             "https://mock.invalid/two.bin");
    ASSERT_THAT(f.get(), NotNull());
  }
  server.ResetCounts();

  // Reopen hits cache (m_handle == nullptr) and destroys with m_handle == null.
  {
    auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                             "https://mock.invalid/two.bin");
    ASSERT_THAT(f.get(), NotNull());
    std::vector<uint8_t> head(16);
    EXPECT_EQ(16u, f->read(head.data(), head.size()));
    EXPECT_EQ(0, server.open_count());
  }

  // Reopen hits cache (m_handle == nullptr), then reading into chunk 1
  // triggers lazy networking.open() inside NetworkFile::read().
  {
    auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                             "https://mock.invalid/two.bin");
    ASSERT_THAT(f.get(), NotNull());
    std::vector<uint8_t> out(two_chunks.size());
    EXPECT_EQ(two_chunks.size(), f->read(out.data(), out.size()));
    EXPECT_EQ(two_chunks, out);
    EXPECT_EQ(1, server.open_count());
  }

  // Also test lazy open failure inside NetworkFile::read() when file vanishes.
  const std::vector<uint8_t> two_chunks_b =
      MakePatternData(kChunkSize + 100, 2);
  server.AddBytes("https://mock.invalid/vanish.bin", two_chunks_b);
  {
    auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                             "https://mock.invalid/vanish.bin");
    ASSERT_THAT(f.get(), NotNull());
  }
  server.AddFile("https://mock.invalid/vanish.bin",
                 {.data = two_chunks_b, .fail_open = true});
  {
    auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                             "https://mock.invalid/vanish.bin");
    ASSERT_THAT(f.get(), NotNull());
    // Reading chunk 1 lazily calls open -> fail_open -> returns 0.
    std::vector<uint8_t> out(16);
    ASSERT_TRUE(f->seek(kChunkSize, SEEK_SET));
    EXPECT_EQ(0u, f->read(out.data(), out.size()));
    EXPECT_EQ(PROJ_ERR_OTHER_NETWORK_ERROR, proj_context_errno(ctx.get()));
    proj_context_errno_set(ctx.get(), 0);
  }

  // Test reassign_context warning when close callback pointer differs.
  {
    ScopedContext ctx2 = MakeContext();
    ASSERT_THAT(ctx2.get(), NotNull());
    ScopedLogCapture logs2(ctx2.get(), PJ_LOG_ERROR);
    ASSERT_TRUE(server.Install(ctx2.get()));

    ScopedContext ctx3 = MakeContext();
    ASSERT_THAT(ctx3.get(), NotNull());
    ScopedLogCapture logs3(ctx3.get(), PJ_LOG_ERROR);
    ASSERT_EQ(1, proj_context_set_network_callbacks(
                     ctx3.get(), ctx->networking.open, &AltCloseCallback,
                     ctx->networking.get_header_value,
                     ctx->networking.read_range, &server));

    auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                             "https://mock.invalid/two.bin");
    ASSERT_THAT(f.get(), NotNull());
    f->reassign_context(ctx2.get());
    EXPECT_FALSE(logs2.ContainsMessage("Networking close callback"));

    f->reassign_context(ctx3.get());
    EXPECT_TRUE(logs3.ContainsMessage("Networking close callback has changed"));
    // Restore original context before leaving block.
    f->reassign_context(ctx.get());
  }
}

TEST_F(NetworkFileManagerTest,
       DiskCacheReuseInvalidationEvictionTtlAndCorruptRows) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
  ScopedNetworkWrapper wrapper(true);
  TempDir dir("nfm_disk_cache");
  proj_context_set_user_writable_directory(ctx.get(), dir.c_str(), 1);
  const std::string db_path = dir.child("cache.db").string();
  proj_grid_cache_set_filename(ctx.get(), db_path.c_str());

  MockNetworkServer server;
  ASSERT_TRUE(server.Install(ctx.get()));

  // 1. Insert 3 chunks for urlA (`tail -> mid -> head` -> move_to_tail tests
  // all three positions when urlA properties change!).
  const std::vector<uint8_t> data_a1 = MakePatternData(3 * kChunkSize, 1);
  server.AddBytes("https://mock.invalid/a.bin", data_a1, "\"a1\"");
  {
    auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                             "https://mock.invalid/a.bin");
    ASSERT_THAT(f.get(), NotNull());
    std::vector<uint8_t> buf(data_a1.size());
    ASSERT_EQ(buf.size(), f->read(buf.data(), buf.size()));
    EXPECT_EQ(data_a1, buf);
  }

  // Clear memory cache, hit SQLite disk cache directly with unlimited TTL (-1)
  // even when lastChecked = 0. Order in DB is [chunk2(head), chunk1, chunk0];
  // opening moves chunk0 (tail) to head -> [0, 2, 1], then reading chunk2 moves
  // middle node to head -> [2, 0, 1], then reading chunk1 moves tail -> head.
  ::NS_PROJ::FileManager::clearMemoryCache();
  {
    ScopedSqliteDb db = OpenSqliteDb(db_path.c_str());
    ASSERT_THAT(db.get(), NotNull());
    ASSERT_TRUE(ExecSql(db.get(), "UPDATE properties SET lastChecked = 0;"));
  }
  proj_grid_cache_set_ttl(ctx.get(), -1);
  server.ResetCounts();
  {
    auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                             "https://mock.invalid/a.bin");
    ASSERT_THAT(f.get(), NotNull());
    uint8_t b = 0;
    ASSERT_TRUE(f->seek(2 * kChunkSize, SEEK_SET));
    ASSERT_EQ(1u, f->read(&b, 1));
    ASSERT_TRUE(f->seek(kChunkSize, SEEK_SET));
    ASSERT_EQ(1u, f->read(&b, 1));
    EXPECT_EQ(0, server.open_count());
  }

  // 2. TTL expiration WITHOUT server change updates existing `(url, 0)` row
  // (mainRet == SQLITE_ROW branch in NetworkChunkCache::insert). Then read
  // chunks 1 and 2 so DB LRU list order becomes [2(id3)=head, 1(id2)=mid,
  // 0(id1)=tail] before Section 3 invalidates ids [1, 2, 3].
  ::NS_PROJ::FileManager::clearMemoryCache();
  proj_grid_cache_set_ttl(ctx.get(), 1);
  {
    ScopedSqliteDb db = OpenSqliteDb(db_path.c_str());
    ASSERT_THAT(db.get(), NotNull());
    ASSERT_TRUE(ExecSql(db.get(), "UPDATE properties SET lastChecked = 0;"));
  }
  {
    auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                             "https://mock.invalid/a.bin");
    ASSERT_THAT(f.get(), NotNull());
    uint8_t b = 0;
    ASSERT_TRUE(f->seek(kChunkSize, SEEK_SET));
    ASSERT_EQ(1u, f->read(&b, 1));
    ASSERT_TRUE(f->seek(2 * kChunkSize, SEEK_SET));
    ASSERT_EQ(1u, f->read(&b, 1));
  }
  EXPECT_EQ(1, server.open_count());

  // 3. TTL expiration WITH changed server size/ETag invalidates all 3 chunks of
  // urlA (ids [1, 2, 3] exercise move_to_tail on tail, middle, and head) and
  // reuses an invalidated tail slot immediately.
  const std::vector<uint8_t> data_a2 = MakePatternData(3 * kChunkSize + 10, 5);
  server.AddFile("https://mock.invalid/a.bin",
                 {.data = data_a2, .etag = "", .last_modified = ""});
  ::NS_PROJ::FileManager::clearMemoryCache();
  {
    ScopedSqliteDb db = OpenSqliteDb(db_path.c_str());
    ASSERT_THAT(db.get(), NotNull());
    ASSERT_TRUE(ExecSql(db.get(), "UPDATE properties SET lastChecked = 0;"));
  }
  // Open urlA -> detects change, invalidates 3 chunks, updates properties NULL,
  // reuses one invalidated slot immediately for chunk 0 of a.bin!
  {
    auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                             "https://mock.invalid/a.bin");
    ASSERT_THAT(f.get(), NotNull());
  }
  // Also insert urlB with empty headers (NULL bind in INSERT properties), then
  // update to non-empty headers on TTL expiration (TEXT bind in UPDATE), and
  // insert urlC to reuse another invalidated chunk slot.
  server.AddFile(
      "https://mock.invalid/b_null.bin",
      {.data = MakePatternData(1024, 9), .etag = "", .last_modified = ""});
  {
    auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                             "https://mock.invalid/b_null.bin");
    ASSERT_THAT(f.get(), NotNull());
  }
  ::NS_PROJ::FileManager::clearMemoryCache();
  server.AddBytes("https://mock.invalid/b_null.bin", MakePatternData(1024, 12),
                  "\"b2\"", "Wed, 03 Jan 2026 00:00:00 GMT");
  {
    ScopedSqliteDb db = OpenSqliteDb(db_path.c_str());
    ASSERT_THAT(db.get(), NotNull());
    ASSERT_TRUE(ExecSql(db.get(), "UPDATE properties SET lastChecked = 0;"));
  }
  {
    auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                             "https://mock.invalid/b_null.bin");
    ASSERT_THAT(f.get(), NotNull());
  }

  // 4. Test LRU eviction via PROJ_GRID_CACHE_MAX_SIZE_BYTES = 32768 (2 chunks).
  proj_grid_cache_clear(ctx.get());
  ::NS_PROJ::FileManager::clearMemoryCache();
  {
    ScopedEnvVar max_bytes("PROJ_GRID_CACHE_MAX_SIZE_BYTES", "32768");
    proj_grid_cache_set_max_size(ctx.get(), 0);
  }
  const std::vector<uint8_t> three_chunks = MakePatternData(3 * kChunkSize, 11);
  server.AddBytes("https://mock.invalid/lru.bin", three_chunks);
  {
    auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                             "https://mock.invalid/lru.bin");
    ASSERT_THAT(f.get(), NotNull());
    std::vector<uint8_t> buf(three_chunks.size());
    ASSERT_EQ(buf.size(), f->read(buf.data(), buf.size()));
    EXPECT_EQ(three_chunks, buf);
  }
  {
    ScopedSqliteDb db = OpenSqliteDb(db_path.c_str());
    ASSERT_THAT(db.get(), NotNull());
    sqlite3_stmt* raw_ptr = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_prepare_v2(db.get(), "SELECT COUNT(*) FROM chunks", -1,
                                 &raw_ptr, nullptr));
    ScopedSqliteStmt stmt(raw_ptr);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(stmt.get()));
    EXPECT_EQ(2, sqlite3_column_int(stmt.get(), 0));
  }

  // 5. Corrupt chunk rows (blob_size < data_size and data_size > chunk size).
  proj_grid_cache_clear(ctx.get());
  ::NS_PROJ::FileManager::clearMemoryCache();
  proj_grid_cache_set_max_size(ctx.get(), 1);
  proj_grid_cache_set_ttl(ctx.get(), 3600);
  server.AddBytes("https://mock.invalid/corrupt.bin", MakePatternData(256, 3));
  {
    auto f = ::NS_PROJ::pj_network_file_open(
        ctx.get(), "https://mock.invalid/corrupt.bin");
    ASSERT_THAT(f.get(), NotNull());
  }

  // Case A: data_size > blob_size -> logs "blob_size=...".
  ::NS_PROJ::FileManager::clearMemoryCache();
  {
    ScopedSqliteDb db = OpenSqliteDb(db_path.c_str());
    ASSERT_THAT(db.get(), NotNull());
    ASSERT_TRUE(
        ExecSql(db.get(), "UPDATE chunks SET data_size = 20000 WHERE id = 1;"));
  }
  logs.Clear();
  EXPECT_THAT(::NS_PROJ::pj_network_file_open(
                  ctx.get(), "https://mock.invalid/corrupt.bin")
                  .get(),
              NotNull());
  EXPECT_TRUE(logs.ContainsMessage("< data_size"));

  // Case B: blob_size >= data_size > DOWNLOAD_CHUNK_SIZE -> logs
  // "data_size > DOWNLOAD_CHUNK_SIZE".
  ::NS_PROJ::FileManager::clearMemoryCache();
  {
    ScopedSqliteDb db = OpenSqliteDb(db_path.c_str());
    ASSERT_THAT(db.get(), NotNull());
    sqlite3_stmt* raw_ptr = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_prepare_v2(db.get(),
                                 "UPDATE chunk_data SET data = ? WHERE id = 1",
                                 -1, &raw_ptr, nullptr));
    ScopedSqliteStmt stmt(raw_ptr);
    std::vector<uint8_t> oversized(20000, 0xAB);
    ASSERT_EQ(SQLITE_OK, sqlite3_bind_blob(stmt.get(), 1, oversized.data(),
                                           static_cast<int>(oversized.size()),
                                           SQLITE_STATIC));
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(stmt.get()));
    ASSERT_TRUE(
        ExecSql(db.get(), "UPDATE chunks SET data_size = 20000 WHERE id = 1;"));
  }
  logs.Clear();
  EXPECT_THAT(::NS_PROJ::pj_network_file_open(
                  ctx.get(), "https://mock.invalid/corrupt.bin")
                  .get(),
              NotNull());
  EXPECT_TRUE(logs.ContainsMessage("data_size > DOWNLOAD_CHUNK_SIZE"));

  // Case C: invalidated chunk with data_id <= 0 logs "data_id <= 0".
  proj_grid_cache_clear(ctx.get());
  ::NS_PROJ::FileManager::clearMemoryCache();
  server.AddBytes("https://mock.invalid/bad_data_id_seed.bin",
                  MakePatternData(64, 1));
  EXPECT_THAT(::NS_PROJ::pj_network_file_open(
                  ctx.get(), "https://mock.invalid/bad_data_id_seed.bin")
                  .get(),
              NotNull());
  {
    ScopedSqliteDb db = OpenSqliteDb(db_path.c_str());
    ASSERT_THAT(db.get(), NotNull());
    ASSERT_TRUE(ExecSql(db.get(),
                        "UPDATE chunks SET url = 'invalidated', offset = -1, "
                        "data_id = 0, data_size = 0 WHERE id = 1;"));
  }
  ::NS_PROJ::FileManager::clearMemoryCache();
  logs.Clear();
  server.AddBytes("https://mock.invalid/bad_data_id_next.bin",
                  MakePatternData(64, 2));
  EXPECT_THAT(::NS_PROJ::pj_network_file_open(
                  ctx.get(), "https://mock.invalid/bad_data_id_next.bin")
                  .get(),
              NotNull());
  EXPECT_TRUE(logs.ContainsMessage("data_id <= 0"));

  // Case D: incomplete schema (`properties` exists but `chunks` /
  // `downloaded_file_properties` missing) exercises prepare() failures.
  proj_grid_cache_clear(ctx.get());
  ::NS_PROJ::FileManager::clearMemoryCache();
  {
    ScopedSqliteDb db = OpenSqliteDb(db_path.c_str());
    ASSERT_THAT(db.get(), NotNull());
    ASSERT_TRUE(ExecSql(db.get(),
                        "CREATE TABLE properties(url TEXT PRIMARY KEY, "
                        "lastChecked INT, fileSize INT, lastModified TEXT, "
                        "etag TEXT);"));
  }
  const std::vector<uint8_t> incomplete_data =
      MakePatternData(2 * kChunkSize, 3);
  server.AddBytes("https://mock.invalid/incomplete.bin", incomplete_data);
  logs.Clear();
  {
    auto f = ::NS_PROJ::pj_network_file_open(
        ctx.get(), "https://mock.invalid/incomplete.bin");
    ASSERT_THAT(f.get(), NotNull());
    uint8_t b = 0;
    ASSERT_TRUE(f->seek(kChunkSize, SEEK_SET));
    EXPECT_EQ(1u, f->read(&b, 1));
    EXPECT_EQ(incomplete_data[kChunkSize], b);
  }
  EXPECT_TRUE(logs.ContainsMessage("no such table"));
  ASSERT_TRUE(WriteTextFile(dir.child("incomplete.bin"), "x"));
  EXPECT_EQ(1, proj_is_download_needed(
                   ctx.get(), "https://mock.invalid/incomplete.bin", 0));
}

TEST_F(NetworkFileManagerTest, DiskChunkCacheOpenFailuresAndLockContention) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  ScopedLogCapture logs(ctx.get(), PJ_LOG_TRACE);
  ScopedNetworkWrapper wrapper(true);
  TempDir dir("nfm_disk_err");
  proj_context_set_user_writable_directory(ctx.get(), dir.c_str(), 1);
  const std::string db_path = dir.child("cache.db").string();
  proj_grid_cache_set_filename(ctx.get(), db_path.c_str());

  MockNetworkServer server;
  server.AddBytes("https://mock.invalid/lock.bin", MakePatternData(128));
  ASSERT_TRUE(server.Install(ctx.get()));

  // 1. Nonexistent custom SQLite VFS fails sqlite3_open_v2 / BEGIN EXCLUSIVE.
  proj_context_set_sqlite3_vfs_name(ctx.get(), "nonexistent_proj_vfs_name");
  EXPECT_THAT(::NS_PROJ::pj_network_file_open(ctx.get(),
                                              "https://mock.invalid/lock.bin")
                  .get(),
              NotNull());
  EXPECT_TRUE(logs.ContainsMessage("no such vfs"));
  proj_context_set_sqlite3_vfs_name(ctx.get(), nullptr);

  // 2. Corrupt non-SQLite file at cache path -> SQLITE_NOTADB (!= SQLITE_BUSY).
  ::NS_PROJ::FileManager::clearMemoryCache();
  ASSERT_TRUE(WriteTextFile(db_path, "this is not a valid sqlite database"));
  logs.Clear();
  EXPECT_THAT(::NS_PROJ::pj_network_file_open(ctx.get(),
                                              "https://mock.invalid/lock.bin")
                  .get(),
              NotNull());
  EXPECT_FALSE(logs.entries().empty());

  // 3. Valid SQLite DB missing `properties` table while `chunk_data` already
  // exists -> createDBStructure() fails cleanly.
  ::NS_PROJ::FileManager::clearMemoryCache();
  std::error_code ec;
  std::filesystem::remove(db_path, ec);
  {
    ScopedSqliteDb db = OpenSqliteDb(db_path.c_str());
    ASSERT_THAT(db.get(), NotNull());
    ASSERT_TRUE(ExecSql(db.get(), "CREATE TABLE chunk_data(id INTEGER);"));
  }
  logs.Clear();
  EXPECT_THAT(::NS_PROJ::pj_network_file_open(ctx.get(),
                                              "https://mock.invalid/lock.bin")
                  .get(),
              NotNull());
  EXPECT_TRUE(logs.ContainsMessage("Creating cache DB structure"));
  std::filesystem::remove(db_path, ec);

  // Seed a clean valid cache DB schema before lock contention tests.
  server.AddBytes("https://mock.invalid/seed.bin", MakePatternData(64));
  EXPECT_THAT(::NS_PROJ::pj_network_file_open(ctx.get(),
                                              "https://mock.invalid/seed.bin")
                  .get(),
              NotNull());

  // 4. Exclusive lock timeout with PROJ_LOCK_MAX_ITERS=1 exercises sleep_ms()
  // and SQLITE_BUSY timeout path. Single-threaded test for TSan safety.
  ::NS_PROJ::FileManager::clearMemoryCache();
  {
    ScopedSqliteDb db = OpenSqliteDb(db_path.c_str());
    ASSERT_THAT(db.get(), NotNull());
    ASSERT_TRUE(ExecSql(db.get(), "BEGIN EXCLUSIVE;"));
    ScopedEnvVar max_iters("PROJ_LOCK_MAX_ITERS", "1");
    logs.Clear();
    EXPECT_THAT(::NS_PROJ::pj_network_file_open(ctx.get(),
                                                "https://mock.invalid/lock.bin")
                    .get(),
                NotNull());
    EXPECT_TRUE(logs.ContainsMessage("Lock taken on cache"));
    EXPECT_TRUE(logs.ContainsMessage("Cannot take exclusive lock"));
    ASSERT_TRUE(ExecSql(db.get(), "COMMIT;"));
  }

  // 5. Transient lock contention from another thread recovers after sleep_ms().
  ::NS_PROJ::FileManager::clearMemoryCache();
  {
    std::mutex lock_mu;  // NOLINT(build/c++11)
    std::condition_variable lock_cv;
    bool lock_acquired = false;
    std::jthread holder([&] {
      ScopedSqliteDb db = OpenSqliteDb(db_path.c_str());
      if (db != nullptr && ExecSql(db.get(), "BEGIN EXCLUSIVE;")) {
        {
          std::lock_guard<std::mutex> lk(lock_mu);  // NOLINT(build/c++11)
          lock_acquired = true;
        }
        lock_cv.notify_one();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        ExecSql(db.get(), "COMMIT;");
      } else {
        std::lock_guard<std::mutex> lk(lock_mu);  // NOLINT(build/c++11)
        lock_acquired = true;
        lock_cv.notify_one();
      }
    });
    {
      std::unique_lock<std::mutex> lk(lock_mu);  // NOLINT(build/c++11)
      lock_cv.wait(lk, [&] { return lock_acquired; });
    }
    EXPECT_THAT(::NS_PROJ::pj_network_file_open(ctx.get(),
                                                "https://mock.invalid/lock.bin")
                    .get(),
                NotNull());
  }
}

TEST_F(NetworkFileManagerTest, DiskChunkCacheConsistencyCheckAllBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  ScopedLogSuppressor silence(ctx.get());
  ScopedNetworkWrapper wrapper(true);
  ScopedEnvVar check_consistency("PROJ_CHECK_CACHE_CONSISTENCY", "1");
  TempDir dir("nfm_consistency");
  proj_context_set_user_writable_directory(ctx.get(), dir.c_str(), 1);
  const std::string db_path = dir.child("cache.db").string();
  proj_grid_cache_set_filename(ctx.get(), db_path.c_str());

  MockNetworkServer server;
  server.AddBytes("https://mock.invalid/c.bin",
                  MakePatternData(3 * kChunkSize));
  ASSERT_TRUE(server.Install(ctx.get()));

  // Healthy empty DB check + populate 3 chunks, then clear checks healthy
  // chain.
  proj_grid_cache_clear(ctx.get());
  ::NS_PROJ::FileManager::clearMemoryCache();
  {
    auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                             "https://mock.invalid/c.bin");
    ASSERT_THAT(f.get(), NotNull());
    std::vector<uint8_t> buf(3 * kChunkSize);
    ASSERT_EQ(buf.size(), f->read(buf.data(), buf.size()));
  }
  // Check consistency runs on non-empty DB during open in
  // proj_grid_cache_clear.
  proj_grid_cache_clear(ctx.get());

  struct CorruptCase {
    const char* name;
    int chunks_to_seed;
    const char* sql;
  };
  const CorruptCase kCases[] = {
      {.name = "orphan_chunk_data",
       .chunks_to_seed = 1,
       .sql = "INSERT INTO chunk_data(id, data) VALUES (99, 'x');"},
      {.name = "orphan_chunk",
       .chunks_to_seed = 1,
       .sql = "DELETE FROM linked_chunks; "
              "UPDATE linked_chunks_head_tail SET head = NULL, tail = NULL;"},
      {.name = "chunk_missing_property",
       .chunks_to_seed = 1,
       .sql = "DELETE FROM properties;"},
      {.name = "empty_head_tail",
       .chunks_to_seed = 1,
       .sql = "DELETE FROM linked_chunks_head_tail;"},
      {.name = "duplicate_head_tail",
       .chunks_to_seed = 1,
       .sql = "INSERT INTO linked_chunks_head_tail VALUES (1, 1);"},
      {.name = "missing_head_link_id",
       .chunks_to_seed = 1,
       .sql = "DELETE FROM linked_chunks; DELETE FROM chunks; "
              "DELETE FROM chunk_data; "
              "UPDATE linked_chunks_head_tail SET head = 999, tail = 999;"},
      {.name = "null_head_nonempty_links",
       .chunks_to_seed = 1,
       .sql = "UPDATE linked_chunks_head_tail SET head = NULL;"},
      {.name = "premature_next_null",
       .chunks_to_seed = 2,
       .sql = "UPDATE linked_chunks SET prev = NULL, next = NULL WHERE id = 1; "
              "UPDATE linked_chunks SET prev = 1, next = NULL WHERE id = 2; "
              "UPDATE linked_chunks_head_tail SET head = 1, tail = 2;"},
      {.name = "cycle_on_next",
       .chunks_to_seed = 1,
       .sql = "UPDATE linked_chunks SET next = 1 WHERE id = 1;"},
      {.name = "ghost_item_on_next",
       .chunks_to_seed = 2,
       .sql = "UPDATE linked_chunks SET prev = NULL, next = NULL WHERE id = 1; "
              "UPDATE linked_chunks SET prev = NULL, next = NULL WHERE id = 2; "
              "UPDATE linked_chunks_head_tail SET head = 1, tail = 1;"},
      {.name = "missing_prev_link_id",
       .chunks_to_seed = 2,
       .sql = "UPDATE linked_chunks SET prev = NULL, next = 2 WHERE id = 1; "
              "UPDATE linked_chunks SET prev = 999, next = NULL WHERE id = 2; "
              "UPDATE linked_chunks_head_tail SET head = 1, tail = 2;"},
      {.name = "premature_prev_null",
       .chunks_to_seed = 2,
       .sql = "UPDATE linked_chunks SET prev = NULL, next = 2 WHERE id = 1; "
              "UPDATE linked_chunks SET prev = NULL, next = NULL WHERE id = 2; "
              "UPDATE linked_chunks_head_tail SET head = 1, tail = 2;"},
      {.name = "cycle_on_prev",
       .chunks_to_seed = 2,
       .sql = "UPDATE linked_chunks SET prev = NULL, next = 2 WHERE id = 1; "
              "UPDATE linked_chunks SET prev = 2, next = NULL WHERE id = 2; "
              "UPDATE linked_chunks_head_tail SET head = 1, tail = 2;"},
      {.name = "ghost_item_on_prev",
       .chunks_to_seed = 3,
       .sql = "UPDATE linked_chunks SET prev = NULL, next = 2 WHERE id = 1; "
              "UPDATE linked_chunks SET prev = 1, next = 3 WHERE id = 2; "
              "UPDATE linked_chunks SET prev = 1, next = NULL WHERE id = 3; "
              "UPDATE linked_chunks_head_tail SET head = 1, tail = 3;"},
  };

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    ::NS_PROJ::FileManager::clearMemoryCache();
    {
      auto f = ::NS_PROJ::pj_network_file_open(ctx.get(),
                                               "https://mock.invalid/c.bin");
      ASSERT_THAT(f.get(), NotNull());
      if (c.chunks_to_seed == 2) {
        // Non-sequential jump to chunk 2 avoids prefetching chunk 1, yielding
        // exactly 2 rows in chunks/linked_chunks (ids 1 and 2).
        uint8_t b = 0;
        ASSERT_TRUE(f->seek(2 * kChunkSize, SEEK_SET));
        ASSERT_EQ(1u, f->read(&b, 1));
      } else if (c.chunks_to_seed > 2) {
        std::vector<uint8_t> buf(static_cast<size_t>(c.chunks_to_seed) *
                                 kChunkSize);
        ASSERT_EQ(buf.size(), f->read(buf.data(), buf.size()));
      }
    }
    {
      ScopedSqliteDb db = OpenSqliteDb(db_path.c_str());
      ASSERT_THAT(db.get(), NotNull());
      ASSERT_TRUE(ExecSql(db.get(), c.sql));
    }
    // Triggers DiskChunkCache::initialize() -> checkConsistency() and cleans
    // up.
    proj_grid_cache_clear(ctx.get());
  }
}

TEST_F(NetworkFileManagerTest, DownloadAndIsDownloadNeededAllPaths) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
  TempDir dir("nfm_download");
  proj_context_set_user_writable_directory(ctx.get(), dir.c_str(), 1);
  const std::string db_path = dir.child("cache.db").string();
  proj_grid_cache_set_filename(ctx.get(), db_path.c_str());

  // 1. Network disabled errors for both proj_is_download_needed & download.
  EXPECT_EQ(0, proj_is_download_needed(ctx.get(), "https://mock.invalid/g", 0));
  EXPECT_EQ(0, proj_download_file(ctx.get(), "https://mock.invalid/g", 0,
                                  nullptr, nullptr));
  EXPECT_TRUE(logs.ContainsMessage("Networking capabilities are not enabled"));

  ScopedNetworkWrapper wrapper(true);
  MockNetworkServer server;
  ASSERT_TRUE(server.Install(ctx.get()));

  // 2. Endpoint handling and build_url branches.
  proj_context_set_url_endpoint(ctx.get(), "");
  EXPECT_EQ(0, proj_is_download_needed(ctx.get(), "bare_file.bin", 0));
  EXPECT_EQ(
      1, proj_download_file(ctx.get(), "bare_file.bin", 0, nullptr, nullptr));

  proj_context_set_url_endpoint(ctx.get(), "https://mock.invalid");
  server.AddBytes("https://mock.invalid/bare_ep.bin", MakePatternData(100));
  EXPECT_EQ(1, proj_is_download_needed(ctx.get(), "bare_ep.bin", 0));
  EXPECT_EQ(1,
            proj_download_file(ctx.get(), "bare_ep.bin", 0, nullptr, nullptr));
  EXPECT_TRUE(std::filesystem::exists(dir.child("bare_ep.bin")));

  proj_context_set_url_endpoint(ctx.get(), "https://mock.invalid/");
  server.AddBytes("https://mock.invalid/http_ep.bin", MakePatternData(80));
  EXPECT_EQ(1, proj_is_download_needed(ctx.get(), "http_ep.bin", 0));
  EXPECT_EQ(1, proj_is_download_needed(ctx.get(),
                                       "http://mock.invalid/plain.bin", 0));
  EXPECT_EQ(1, proj_is_download_needed(ctx.get(), "~/tilde.bin", 0));
  EXPECT_EQ(1, proj_is_download_needed(ctx.get(), "/abs/path.bin", 0));
  EXPECT_EQ(1, proj_is_download_needed(ctx.get(), "./rel/path.bin", 0));
  EXPECT_EQ(1, proj_is_download_needed(ctx.get(), "../up/path.bin", 0));
  EXPECT_EQ(1, proj_is_download_needed(ctx.get(), "C:/win/path.bin", 0));

  // 3. Eviction of negative lookup cache + dbContext->invalidateGridInfo().
  server.AddBytes("https://mock.invalid/evict_test.gtx", MakePatternData(64));
  char found_buf[512] = {};
  EXPECT_EQ(0, pj_find_file(ctx.get(), "evict_test.gtx", found_buf,
                            sizeof(found_buf), true));
  EXPECT_NE(ctx->lookupedFiles.end(),
            ctx->lookupedFiles.find("evict_test.gtx"));
  EXPECT_EQ(
      1, proj_download_file(ctx.get(), "https://mock.invalid/evict_test.gtx", 0,
                            nullptr, nullptr));
  EXPECT_EQ(1, pj_find_file(ctx.get(), "evict_test.gtx", found_buf,
                            sizeof(found_buf), true));

  // 4. Multi-chunk download using PROJ_FULL_FILE_CHUNK_SIZE=100 & progress cbk.
  const std::vector<uint8_t> chunked_bytes = MakePatternData(250, 4);
  server.AddFile("https://mock.invalid/multi.bin",
                 {.data = chunked_bytes, .etag = "", .last_modified = ""});
  {
    ScopedEnvVar chunk_env("PROJ_FULL_FILE_CHUNK_SIZE", "100");
    struct ProgressState {
      std::vector<double> pcts;
      bool cancel_immediately = false;
    } state;
    auto progress_cbk = [](PJ_CONTEXT*, double pct, void* ud) -> int {
      auto* s = static_cast<ProgressState*>(ud);
      s->pcts.push_back(pct);
      return s->cancel_immediately ? 0 : 1;
    };

    // First test cancellation on first progress callback.
    state.cancel_immediately = true;
    EXPECT_EQ(0, proj_download_file(ctx.get(), "https://mock.invalid/multi.bin",
                                    0, progress_cbk, &state));
    EXPECT_FALSE(std::filesystem::exists(dir.child("multi.bin")));

    // Now let multi-chunk download complete cleanly.
    state.pcts.clear();
    state.cancel_immediately = false;
    EXPECT_EQ(1, proj_download_file(ctx.get(), "https://mock.invalid/multi.bin",
                                    0, progress_cbk, &state));
    EXPECT_TRUE(std::filesystem::exists(dir.child("multi.bin")));
    ASSERT_EQ(2u, state.pcts.size());
    EXPECT_DOUBLE_EQ(200.0 / 250.0, state.pcts[0]);
    EXPECT_DOUBLE_EQ(1.0, state.pcts[1]);
  }

  // 5. Existing local file edge cases in proj_is_download_needed().
  // Case A: cache disabled -> returns 0 even if local file exists.
  proj_grid_cache_set_enable(ctx.get(), 0);
  EXPECT_EQ(0, proj_is_download_needed(ctx.get(),
                                       "https://mock.invalid/multi.bin", 0));
  proj_grid_cache_set_enable(ctx.get(), 1);

  // Case B: manually placed file without downloaded_file_properties -> 1.
  ASSERT_TRUE(WriteTextFile(dir.child("manual.bin"), "manual content"));
  EXPECT_EQ(1, proj_is_download_needed(ctx.get(),
                                       "https://mock.invalid/manual.bin", 0));

  // Case C: downloaded file + valid or negative TTL -> 0 without server calls.
  server.ResetCounts();
  EXPECT_EQ(0, proj_is_download_needed(ctx.get(),
                                       "https://mock.invalid/multi.bin", 0));
  proj_grid_cache_set_ttl(ctx.get(), -1);
  EXPECT_EQ(0, proj_is_download_needed(ctx.get(),
                                       "https://mock.invalid/multi.bin", 0));
  EXPECT_EQ(1, proj_download_file(ctx.get(), "https://mock.invalid/multi.bin",
                                  0, nullptr, nullptr));
  EXPECT_EQ(0, server.open_count());

  // Case D: TTL expired (lastChecked = 0) with:
  //   (i) open failure -> returns 0,
  //   (ii) missing Content-Range -> returns 0,
  //   (iii) unchanged props -> updates lastChecked and returns 0,
  //   (iv) changed props -> returns 1 -> re-download updates table & file.
  proj_grid_cache_set_ttl(ctx.get(), 1);
  auto expire_downloaded = [&] {
    ScopedSqliteDb db = OpenSqliteDb(db_path.c_str());
    ASSERT_THAT(db.get(), NotNull());
    ASSERT_TRUE(ExecSql(
        db.get(), "UPDATE downloaded_file_properties SET lastChecked = 0;"));
  };

  expire_downloaded();
  server.AddFile("https://mock.invalid/multi.bin",
                 {.data = chunked_bytes, .fail_open = true});
  EXPECT_EQ(0, proj_is_download_needed(ctx.get(),
                                       "https://mock.invalid/multi.bin", 0));

  expire_downloaded();
  server.AddFile("https://mock.invalid/multi.bin",
                 {.data = chunked_bytes, .omit_content_range = true});
  EXPECT_EQ(0, proj_is_download_needed(ctx.get(),
                                       "https://mock.invalid/multi.bin", 0));

  expire_downloaded();
  server.AddFile("https://mock.invalid/multi.bin",
                 {.data = chunked_bytes, .etag = "", .last_modified = ""});
  EXPECT_EQ(0, proj_is_download_needed(ctx.get(),
                                       "https://mock.invalid/multi.bin", 0));

  // Verify ignore_ttl_setting = 1 skips the TTL block in
  // proj_is_download_needed when the file already exists in cache.
  const std::vector<uint8_t> updated_bytes = MakePatternData(300, 9);
  server.AddBytes("https://mock.invalid/multi.bin", updated_bytes, "\"v2\"",
                  "Tue, 02 Jan 2026 00:00:00 GMT");
  EXPECT_EQ(0,
            proj_is_download_needed(ctx.get(), "https://mock.invalid/multi.bin",
                                    /*ignore_ttl_setting=*/1));
  EXPECT_EQ(1, proj_download_file(ctx.get(), "https://mock.invalid/multi.bin",
                                  /*ignore_ttl_setting=*/1, nullptr, nullptr));

  // Test TTL expired + changed props detection with ignore_ttl = 0.
  expire_downloaded();
  server.AddBytes("https://mock.invalid/multi.bin", MakePatternData(310, 10),
                  "\"v3\"", "");
  EXPECT_EQ(1, proj_is_download_needed(ctx.get(),
                                       "https://mock.invalid/multi.bin", 0));
  EXPECT_EQ(1, proj_download_file(ctx.get(), "https://mock.invalid/multi.bin",
                                  0, nullptr, nullptr));
  EXPECT_EQ(310u, std::filesystem::file_size(dir.child("multi.bin")));
}

TEST_F(NetworkFileManagerTest, DownloadFileFaultyFileApiFailurePaths) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
  ScopedNetworkWrapper wrapper(true);

  FaultyMemoryFileSystem mem_fs;
  ASSERT_TRUE(mem_fs.Install(ctx.get()));
  TempFile cache_db("nfm_faulty_cache.db");
  proj_grid_cache_set_filename(ctx.get(), cache_db.c_str());
  proj_context_set_user_writable_directory(ctx.get(), "/mem/writable", 1);

  MockNetworkServer server;
  ASSERT_TRUE(server.Install(ctx.get()));

  const char* kUrl = "https://mock.invalid/fault.bin";
  const std::vector<uint8_t> data = MakePatternData(250, 1);
  ScopedEnvVar chunk_env("PROJ_FULL_FILE_CHUNK_SIZE", "100");

  // 1. CREATE fails.
  server.AddBytes(kUrl, data);
  mem_fs.fail_create = true;
  EXPECT_EQ(0, proj_download_file(ctx.get(), kUrl, 0, nullptr, nullptr));
  EXPECT_TRUE(logs.ContainsMessage("Cannot create"));
  mem_fs.ResetFaults();

  // 2. Network open fails.
  server.AddFile(kUrl, {.data = data, .fail_open = true, .open_error = "500"});
  EXPECT_EQ(0, proj_download_file(ctx.get(), kUrl, 0, nullptr, nullptr));
  mem_fs.ResetFaults();

  // 3. Missing headers fails.
  server.AddFile(kUrl, {.data = data, .omit_content_range = true});
  EXPECT_EQ(0, proj_download_file(ctx.get(), kUrl, 0, nullptr, nullptr));
  mem_fs.ResetFaults();

  // 4. Initial size_read == 0 fails.
  server.AddFile(kUrl, {.data = data, .force_zero_open_read = true});
  logs.Clear();
  EXPECT_EQ(0, proj_download_file(ctx.get(), kUrl, 0, nullptr, nullptr));
  EXPECT_TRUE(logs.ContainsMessage("Did not get as many bytes as expected"));
  mem_fs.ResetFaults();

  // 5. Initial write fails.
  server.AddBytes(kUrl, data);
  mem_fs.fail_write_on_call = 1;
  logs.Clear();
  EXPECT_EQ(0, proj_download_file(ctx.get(), kUrl, 0, nullptr, nullptr));
  EXPECT_TRUE(logs.ContainsMessage("Write error"));
  mem_fs.ResetFaults();

  // 6. Short read during read_range loop fails.
  server.AddFile(kUrl, {.data = data, .max_read_range_bytes = 50});
  logs.Clear();
  EXPECT_EQ(0, proj_download_file(ctx.get(), kUrl, 0, nullptr, nullptr));
  EXPECT_TRUE(logs.ContainsMessage("Did not get as many bytes as expected"));
  mem_fs.ResetFaults();

  // 7. Subsequent write during read_range loop fails.
  server.AddBytes(kUrl, data);
  mem_fs.fail_write_on_call = 2;
  logs.Clear();
  EXPECT_EQ(0, proj_download_file(ctx.get(), kUrl, 0, nullptr, nullptr));
  EXPECT_TRUE(logs.ContainsMessage("Write error"));
  mem_fs.ResetFaults();

  // 8. Rename fails.
  server.AddBytes(kUrl, data);
  mem_fs.fail_rename = true;
  logs.Clear();
  EXPECT_EQ(0, proj_download_file(ctx.get(), kUrl, 0, nullptr, nullptr));
  EXPECT_TRUE(logs.ContainsMessage("Cannot rename"));
  mem_fs.ResetFaults();

  // 9. Disk cache disabled at end returns 0.
  proj_grid_cache_set_enable(ctx.get(), 0);
  EXPECT_EQ(0, proj_download_file(ctx.get(), kUrl, 0, nullptr, nullptr));
}

TEST_F(NetworkFileManagerTest, RemoteVGridShiftAndAssignContextIntegration) {
  constexpr float kShiftM = 2.5f;
  const std::vector<float> shifts(9, kShiftM);
  const std::vector<uint8_t> gtx_bytes =
      BuildGtxGrid(-10.0, 40.0, 10.0, 10.0, 3, 3, shifts);
  ASSERT_FALSE(gtx_bytes.empty());

  // Local oracle via InMemoryFileSystem.
  ScopedContext local_ctx = MakeContext();
  ASSERT_THAT(local_ctx.get(), NotNull());
  InMemoryFileSystem mem_fs;
  mem_fs.AddFile("/mem/oracle.gtx", gtx_bytes);
  ASSERT_TRUE(mem_fs.Install(local_ctx.get()));
  ScopedPj local_pj(
      proj_create(local_ctx.get(), "+proj=vgridshift +grids=/mem/oracle.gtx"));
  ASSERT_THAT(local_pj.get(), NotNull());

  const PJ_COORD pt = proj_coord(0.0, 50.0 * kDegToRad, 100.0, 0.0);
  const PJ_COORD local_out = proj_trans(local_pj.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(local_pj.get()));

  // Remote execution via MockNetworkServer + TempDir cache. Declare all
  // contexts and server before remote_pj so proj_destroy(remote_pj) runs
  // before remote_ctx2 and server are destroyed.
  TempDir dir("nfm_vgridshift");
  ScopedNetworkWrapper wrapper(true);
  MockNetworkServer server;
  server.AddBytes("https://mock.invalid/test_remote.gtx", gtx_bytes);

  ScopedContext remote_ctx = MakeContext();
  ASSERT_THAT(remote_ctx.get(), NotNull());
  ScopedLogSuppressor silence_remote(remote_ctx.get());
  proj_context_set_user_writable_directory(remote_ctx.get(), dir.c_str(), 1);
  ASSERT_TRUE(server.Install(remote_ctx.get()));

  ScopedContext remote_ctx2 = MakeContext();
  ASSERT_THAT(remote_ctx2.get(), NotNull());
  ScopedLogSuppressor silence_remote2(remote_ctx2.get());
  proj_context_set_user_writable_directory(remote_ctx2.get(), dir.c_str(), 1);
  ASSERT_TRUE(server.Install(remote_ctx2.get()));

  ScopedPj remote_pj(proj_create(
      remote_ctx.get(),
      "+proj=vgridshift +grids=https://mock.invalid/test_remote.gtx"));
  ASSERT_THAT(remote_pj.get(), NotNull());
  const PJ_COORD remote_out = proj_trans(remote_pj.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(remote_pj.get()));
  EXPECT_DOUBLE_EQ(local_out.xyz.z, remote_out.xyz.z);

  // Reassign context cleanly with matching network callbacks.
  proj_assign_context(remote_pj.get(), remote_ctx2.get());
  proj_errno_reset(remote_pj.get());

  const PJ_COORD pt2 =
      proj_coord(-2.0 * kDegToRad, 48.0 * kDegToRad, 50.0, 0.0);
  const PJ_COORD local_out2 = proj_trans(local_pj.get(), PJ_FWD, pt2);
  const PJ_COORD remote_out2 = proj_trans(remote_pj.get(), PJ_FWD, pt2);
  EXPECT_EQ(0, proj_errno(remote_pj.get()));
  EXPECT_DOUBLE_EQ(local_out2.xyz.z, remote_out2.xyz.z);
  remote_pj.reset();
}

}  // namespace
}  // namespace proj
