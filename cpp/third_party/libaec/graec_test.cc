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
// Tests libaec CLI frontend binary //third_party/libaec:graec_unsafe
// (third_party/libaec/src/graec.c).
// Note:
//   - Always pass -b 64 (or -b 16) so graec_unsafe does not allocate up to
//     80 MiB (2 buffers x 4 B/sample x 10 MiB for -n 32) per test invocation.
//   - Preserves inherited environ variables (GCOV_PREFIX, LD_LIBRARY_PATH,
//     etc.) so Blaze gcov instrumentation collects coverage from children.
//   - Only for subprocesses expected to fail after fopen()/aec_*_init() (where
//     graec.c jumps to DESTRUCT without closing files or calling aec_*_end()),
//     leak detection is suppressed in the child environment.
//
// See also:
//   https://gitlab.dkrz.de/k202009/libaec/-/blob/main/src/graec.c

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>  // NOLINT
#include <fstream>
#include <initializer_list>
#include <ios>
#include <iterator>
#include <span>  // NOLINT
#include <string>
#include <string_view>
#include <system_error>  // NOLINT
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

extern char** environ;

namespace libaec {
namespace {

using ::testing::ElementsAreArray;
using ::testing::HasSubstr;

struct GraecResult {
  bool exited = false;
  int exit_code = -1;
  std::string stderr_text;
};

class ScopedTempFile {
 public:
  explicit ScopedTempFile(std::string_view suffix) {
    static std::atomic<uint64_t> counter{0};
    const uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);
    path_ = std::filesystem::path(::testing::TempDir()) /
            ("graec_test_" + std::to_string(id) + "_" + std::string(suffix));
  }

  ~ScopedTempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  ScopedTempFile(const ScopedTempFile&) = delete;
  ScopedTempFile& operator=(const ScopedTempFile&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::string string() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

std::filesystem::path GraecBinaryPath() {
  return std::filesystem::path(::testing::SrcDir()) /
         "google3/third_party/libaec/graec_unsafe";
}

void WriteBytes(const std::filesystem::path& path,
                std::span<const uint8_t> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << path;
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(out.good()) << path;
}

std::vector<uint8_t> ReadBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.is_open()) << path;
  if (!in.is_open()) {
    return {};
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());
}

GraecResult RunGraec(std::span<const std::string_view> args,
                     bool suppress_leaks = false) {
  GraecResult result;
  int pipefd[2] = {-1, -1};
  if (pipe2(pipefd, O_CLOEXEC) != 0) {
    ADD_FAILURE() << "pipe2() failed: errno=" << errno;
    return result;
  }
  bool read_open = true;
  bool write_open = true;
  absl::Cleanup close_pipes = [&] {
    if (read_open) close(pipefd[0]);
    if (write_open) close(pipefd[1]);
  };

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    ADD_FAILURE() << "posix_spawn_file_actions_init() failed";
    return result;
  }
  absl::Cleanup destroy_actions = [&] {
    posix_spawn_file_actions_destroy(&actions);
  };

  if (posix_spawn_file_actions_addclose(&actions, pipefd[0]) != 0 ||
      posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO) !=
          0 ||
      posix_spawn_file_actions_addclose(&actions, pipefd[1]) != 0) {
    ADD_FAILURE() << "posix_spawn_file_actions configuration failed";
    return result;
  }

  const std::string exe = GraecBinaryPath().string();
  std::vector<std::string> argv_storage;
  argv_storage.reserve(args.size() + 1);
  argv_storage.push_back(exe);
  for (std::string_view arg : args) {
    argv_storage.emplace_back(arg);
  }
  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(argv_storage.size() + 1);
  for (std::string& s : argv_storage) {
    argv_ptrs.push_back(s.data());
  }
  argv_ptrs.push_back(nullptr);

  // Preserve inherited environment so Blaze coverage (GCOV_PREFIX,
  // LD_LIBRARY_PATH, etc.) continues working in child processes.
  std::vector<std::string> env_storage;
  bool saw_asan = false;
  bool saw_lsan = false;
  for (char** ep = environ; ep != nullptr && *ep != nullptr; ++ep) {
    const std::string_view entry(*ep);
    if (suppress_leaks && entry.starts_with("ASAN_OPTIONS=")) {
      env_storage.push_back(std::string(entry) + ":detect_leaks=0");
      saw_asan = true;
      continue;
    }
    if (suppress_leaks && entry.starts_with("LSAN_OPTIONS=")) {
      env_storage.push_back(std::string(entry) + ":detect_leaks=0");
      saw_lsan = true;
      continue;
    }
    if (suppress_leaks && entry.starts_with("HEAPCHECK=")) {
      continue;
    }
    env_storage.emplace_back(entry);
  }
  if (suppress_leaks) {
    if (!saw_asan) env_storage.emplace_back("ASAN_OPTIONS=detect_leaks=0");
    if (!saw_lsan) env_storage.emplace_back("LSAN_OPTIONS=detect_leaks=0");
    env_storage.emplace_back("HEAPCHECK=");
  }
  std::vector<char*> env_ptrs;
  env_ptrs.reserve(env_storage.size() + 1);
  for (std::string& s : env_storage) {
    env_ptrs.push_back(s.data());
  }
  env_ptrs.push_back(nullptr);

  pid_t pid = -1;
  const int spawn_err = posix_spawn(&pid, exe.c_str(), &actions, nullptr,
                                    argv_ptrs.data(), env_ptrs.data());
  close(pipefd[1]);
  write_open = false;
  if (spawn_err != 0) {
    ADD_FAILURE() << "posix_spawn(" << exe << ") failed: " << spawn_err;
    return result;
  }

  std::array<char, 512> buf{};
  for (;;) {
    const ssize_t n = read(pipefd[0], buf.data(), buf.size());
    if (n > 0) {
      result.stderr_text.append(buf.data(), static_cast<size_t>(n));
    } else if (n == 0 || errno != EINTR) {
      break;
    }
  }
  close(pipefd[0]);
  read_open = false;

  int raw_status = 0;
  pid_t waited = -1;
  do {
    waited = waitpid(pid, &raw_status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited == pid && WIFEXITED(raw_status)) {
    result.exited = true;
    result.exit_code = WEXITSTATUS(raw_status);
  }
  return result;
}

GraecResult RunGraec(std::initializer_list<std::string_view> args,
                     bool suppress_leaks = false) {
  return RunGraec(std::span<const std::string_view>(args), suppress_leaks);
}

void VerifyGraecRoundTrip(std::string_view stem,
                          std::initializer_list<std::string_view> enc_flags,
                          std::initializer_list<std::string_view> dec_flags,
                          std::span<const uint8_t> input) {
  ScopedTempFile raw("raw_" + std::string(stem) + ".bin");
  ScopedTempFile enc("enc_" + std::string(stem) + ".aec");
  ScopedTempFile dec("dec_" + std::string(stem) + ".bin");
  ASSERT_NO_FATAL_FAILURE(WriteBytes(raw.path(), input));

  const std::string raw_path = raw.string();
  const std::string enc_path = enc.string();
  const std::string dec_path = dec.string();

  std::vector<std::string_view> enc_args(enc_flags);
  enc_args.push_back(raw_path);
  enc_args.push_back(enc_path);
  const GraecResult enc_res = RunGraec(enc_args);
  ASSERT_TRUE(enc_res.exited);
  EXPECT_EQ(0, enc_res.exit_code) << enc_res.stderr_text;

  std::vector<std::string_view> dec_args(dec_flags);
  dec_args.push_back("-d");
  dec_args.push_back(enc_path);
  dec_args.push_back(dec_path);
  const GraecResult dec_res = RunGraec(dec_args);
  ASSERT_TRUE(dec_res.exited);
  EXPECT_EQ(0, dec_res.exit_code) << dec_res.stderr_text;
  EXPECT_THAT(ReadBytes(dec.path()), ElementsAreArray(input));
}

TEST(GraecTest, MultiChunkStreamingAndFlagVariantsRoundTrip) {
  // 1. Multi-chunk 8-bit encode/decode with small -b 16 so fread/fwrite loop
  // across multiple iterations plus trailing AEC_FLUSH tail write.
  {
    std::vector<uint8_t> input(80);
    for (size_t i = 0; i < input.size(); ++i) {
      input[i] = static_cast<uint8_t>(i * 3 + 7);
    }
    ASSERT_NO_FATAL_FAILURE(
        VerifyGraecRoundTrip("8", {"-b", "16"}, {"-b", "16"}, input));
  }

  // 2. 16-bit signed MSB with attached -b64 and -j / -r options.
  {
    std::vector<uint8_t> input(64);
    for (size_t i = 0; i < 32; ++i) {
      const int16_t s = static_cast<int16_t>(static_cast<int>(i) * 41 - 600);
      const uint16_t u = static_cast<uint16_t>(s);
      input[2 * i] = static_cast<uint8_t>((u >> 8) & 0xFFu);
      input[2 * i + 1] = static_cast<uint8_t>(u & 0xFFu);
    }
    ASSERT_NO_FATAL_FAILURE(VerifyGraecRoundTrip(
        "16", {"-b64", "-n", "16", "-m", "-s", "-j", "16", "-r", "2"},
        {"-b64", "-n", "16", "-m", "-s", "-j", "16", "-r", "2"}, input));
  }

  // 3. 24-bit 3-byte unpreprocessed (-3 -N), 32-bit (-n 32 -p), and 4-bit
  // restricted (-n 4 -t).
  {
    const std::vector<uint8_t> input24(16 * 3, 0x11);
    ASSERT_NO_FATAL_FAILURE(
        VerifyGraecRoundTrip("24", {"-b", "64", "-n", "24", "-3", "-N"},
                             {"-b", "64", "-n", "24", "-3", "-N"}, input24));

    // ENABLE_RSI_PADDING is disabled on encode in google3; test -p parsing on
    // encode and omit -p on decode for unpadded compressed bytes.
    const std::vector<uint8_t> input32(16 * 4, 0x05);
    ASSERT_NO_FATAL_FAILURE(
        VerifyGraecRoundTrip("32", {"-b", "64", "-n", "32", "-p"},
                             {"-b", "64", "-n", "32"}, input32));

    // Use 4-bit high-contrast values so compressed stream padding stays under
    // 7 bits (7 trailing zero bits with 4-bit ref would decode a trailing ref).
    std::vector<uint8_t> input4(16);
    for (size_t i = 0; i < input4.size(); ++i) {
      input4[i] = static_cast<uint8_t>((i * 3 + 1) & 0x03u);
    }
    ASSERT_NO_FATAL_FAILURE(
        VerifyGraecRoundTrip("4", {"-b", "64", "-n", "4", "-t"},
                             {"-b", "64", "-n", "4", "-t"}, input4));
  }
}

TEST(GraecTest, InvalidArgumentsPrintUsageAndExitZero) {
  auto expect_usage = [](std::initializer_list<std::string_view> args) {
    const GraecResult res = RunGraec(args);
    ASSERT_TRUE(res.exited);
    EXPECT_EQ(0, res.exit_code);
    EXPECT_THAT(res.stderr_text, HasSubstr("SYNOPSIS"));
  };
  expect_usage({});
  expect_usage({"only_one_arg"});
  expect_usage({"not_an_option", "in.bin", "out.aec"});
  expect_usage({"-Z", "in.bin", "out.aec"});
  expect_usage({"-b", "-d", "in.bin", "out.aec"});
  expect_usage({"-j", "-d", "in.bin", "out.aec"});
  expect_usage({"-n", "-d", "in.bin", "out.aec"});
  expect_usage({"-r", "-d", "in.bin", "out.aec"});
}

TEST(GraecTest, FileOpenInitAndStreamErrorsReturnNonzeroExitCodes) {
  // 1. Missing input file returns 99 (fopen failure before opening outfp).
  {
    ScopedTempFile missing("missing_in.bin");
    ScopedTempFile out("out_missing.aec");
    const std::string missing_path = missing.string();
    const std::string out_path = out.string();
    const GraecResult res = RunGraec({"-b", "64", missing_path, out_path});
    ASSERT_TRUE(res.exited);
    EXPECT_EQ(99, res.exit_code);
    EXPECT_THAT(res.stderr_text, HasSubstr("cannot open input file"));
  }

  // 2. Unwritable output path (TempDir directory) returns 99. Suppress leak
  // checking in the child because graec.c jumps to DESTRUCT without
  // fclose(infp).
  {
    ScopedTempFile in("present_in.bin");
    constexpr auto kData = std::to_array<uint8_t>({1, 2, 3, 4});
    ASSERT_NO_FATAL_FAILURE(WriteBytes(in.path(), kData));

    const std::string in_path = in.string();
    const std::string dir_path = ::testing::TempDir();
    const GraecResult res =
        RunGraec({"-b", "64", in_path, dir_path}, /*suppress_leaks=*/true);
    ASSERT_TRUE(res.exited);
    EXPECT_EQ(99, res.exit_code);
    EXPECT_THAT(res.stderr_text, HasSubstr("cannot open output file"));
  }

  // 3. Invalid encoder configuration (-n 33) fails aec_encode_init() -> exit
  // status 255 (-1 mod 256). Suppress child leaks on DESTRUCT exit.
  {
    ScopedTempFile in("bad_init_in.bin");
    ScopedTempFile out("bad_init_out.aec");
    constexpr auto kData = std::to_array<uint8_t>({1, 2, 3, 4});
    ASSERT_NO_FATAL_FAILURE(WriteBytes(in.path(), kData));

    const std::string in_path = in.string();
    const std::string out_path = out.string();
    const GraecResult res =
        RunGraec({"-b", "64", "-n", "33", in_path, out_path},
                 /*suppress_leaks=*/true);
    ASSERT_TRUE(res.exited);
    EXPECT_EQ(255, res.exit_code);
    EXPECT_THAT(res.stderr_text, HasSubstr("initialization failed"));
  }

  // 4. Corrupt compressed stream on decode (-d) fails aec_decode() with
  // AEC_DATA_ERROR (-3 -> 253 mod 256). Suppress child leaks on DESTRUCT exit.
  {
    ScopedTempFile corrupt("corrupt.aec");
    ScopedTempFile out("corrupt_out.bin");
    std::vector<uint8_t> bad(64, 0x00);
    bad[0] = 0b00010000;  // id = 0, low_entropy = 1 (SE) with ref
    bad[13] = 0x80;       // terminates FS at 92 zeros (> SE_TABLE_SIZE == 90)
    ASSERT_NO_FATAL_FAILURE(WriteBytes(corrupt.path(), bad));

    const std::string corrupt_path = corrupt.string();
    const std::string out_path = out.string();
    const GraecResult res = RunGraec({"-d", "-b", "64", corrupt_path, out_path},
                                     /*suppress_leaks=*/true);
    ASSERT_TRUE(res.exited);
    EXPECT_EQ(253, res.exit_code);
    EXPECT_THAT(res.stderr_text, HasSubstr("ERROR: -3"));
  }
}

// TODO(schwehr): Cover malloc OOM exit (in == NULL || out == NULL) and
// defensive unreachable AEC_FLUSH failure check ("while flushing output") if
// allocator/stream fault injection is added.

}  // namespace
}  // namespace libaec
