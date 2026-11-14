// CLI tests for third_party/kakadu/vendor/apps/kdu_compress/kdu_compress.cpp.

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jp2.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jpx.h"
#include "third_party/kakadu/vendor/apps/compressed_io/kdu_file_io.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_arch.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_threads.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

extern char** environ;

namespace kdu_supp {
namespace {

using kdu_core::kdu_byte;
using kdu_core::kdu_codestream;
using kdu_core::kdu_dims;
using kdu_core::kdu_thread_safe_message;
using kdu_core::kdu_uint16;
using kdu_core::kdu_uint32;

// Vendor kdu_threads.cpp:2781,2869 accesses group->wait_managers[thrd_idx]
// out-of-bounds (aborting UBSan under --config=asan-chlor) when add_thread()
// reaches KDU_MAX_THREADS on hosts with > KDU_MAX_THREADS CPUs.
bool HostFitsMaxThreads() {
  return kdu_core::kdu_get_num_processors() <= KDU_MAX_THREADS;
}

class GlobalErrorHandler final : public kdu_thread_safe_message {
 public:
  ~GlobalErrorHandler() noexcept override = default;
  void put_text(const char* s) override {
    if (s != nullptr) std::cerr << s;
  }
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      std::cerr << '\n';
    }
    kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw KDU_ERROR_EXCEPTION;
    }
  }
};

class NoopWarningHandler final : public kdu_core::kdu_message {
 public:
  ~NoopWarningHandler() noexcept override = default;
  void put_text(const char*) override {}
};

class KakaduEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* err = new GlobalErrorHandler();
    static auto* warn = new NoopWarningHandler();
    kdu_core::kdu_customize_errors(err);
    kdu_core::kdu_customize_warnings(warn);
  }
};

const ::testing::Environment* const kKakaduEnv =
    ::testing::AddGlobalTestEnvironment(new KakaduEnvironment());

std::filesystem::path FindBinary(std::string_view rel_path) {
  const std::filesystem::path src_dir(::testing::SrcDir());
  const std::filesystem::path p1 = src_dir / "google3" / rel_path;
  if (std::filesystem::exists(p1)) return p1;
  const std::filesystem::path p2 = src_dir / "_main" / rel_path;
  if (std::filesystem::exists(p2)) return p2;
  return p1;
}

class ScopedTempDir {
 public:
  explicit ScopedTempDir(std::string_view prefix) {
    static std::atomic<int> counter{0};
    const int id = counter.fetch_add(1, std::memory_order_relaxed);
    dir_ = std::filesystem::path(::testing::TempDir()) /
           (std::string(prefix) + "_" + std::to_string(getpid()) + "_" +
            std::to_string(id));
    std::filesystem::create_directories(dir_);
  }
  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;
  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  std::filesystem::path File(std::string_view name) const {
    return dir_ / name;
  }

 private:
  std::filesystem::path dir_;
};

struct RunResult {
  int exit_code = -1;
  std::string stdout_text;
  std::string stderr_text;
};

int64_t MonotonicMillis() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string JoinArgs(const std::vector<std::string>& args) {
  std::string cmd;
  for (const std::string& a : args) {
    if (!cmd.empty()) cmd += ' ';
    cmd += a;
  }
  return cmd;
}

void DrainPipesWithBudget(pid_t pid, int out_fd, int err_fd,
                          std::string& stdout_text, std::string& stderr_text,
                          const std::vector<std::string>& args) {
  constexpr size_t kMaxBytes = 4U * 1024U * 1024U;
  const int64_t deadline_ms = MonotonicMillis() + 10000;
  std::array<struct pollfd, 2> fds{{
      {.fd = out_fd, .events = POLLIN},
      {.fd = err_fd, .events = POLLIN},
  }};
  int open_count = 2;
  std::array<char, 4096> buf{};
  while (open_count > 0) {
    const int64_t now_ms = MonotonicMillis();
    if (now_ms >= deadline_ms ||
        stdout_text.size() + stderr_text.size() > kMaxBytes) {
      kill(pid, SIGKILL);
      ADD_FAILURE() << "Subprocess exceeded timeout/output budget for: "
                    << JoinArgs(args)
                    << "\nstdout prefix: " << stdout_text.substr(0, 512)
                    << "\nstderr prefix: " << stderr_text.substr(0, 512);
      break;
    }
    const int wait_ms =
        static_cast<int>(std::max<int64_t>(1, deadline_ms - now_ms));
    const int prc = poll(fds.data(), fds.size(), wait_ms);
    if (prc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (size_t i = 0; i < fds.size(); ++i) {
      if (fds[i].fd < 0 || fds[i].revents == 0) continue;
      if (fds[i].revents & (POLLIN | POLLHUP)) {
        const ssize_t n = read(fds[i].fd, buf.data(), buf.size());
        if (n > 0) {
          std::string& target = (i == 0) ? stdout_text : stderr_text;
          target.append(buf.data(), static_cast<size_t>(n));
        } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
          fds[i].fd = -1;
          --open_count;
        }
      } else {
        fds[i].fd = -1;
        --open_count;
      }
    }
  }
}

RunResult RunTool(const std::filesystem::path& bin,
                  const std::vector<std::string>& args) {
  RunResult res;
  std::array<int, 2> out_pipe{-1, -1};
  std::array<int, 2> err_pipe{-1, -1};
  absl::Cleanup close_pipes = [&] {
    for (int fd : out_pipe) {
      if (fd >= 0) close(fd);
    }
    for (int fd : err_pipe) {
      if (fd >= 0) close(fd);
    }
  };
  if (pipe(out_pipe.data()) != 0 || pipe(err_pipe.data()) != 0) {
    ADD_FAILURE() << "pipe() failed";
    return res;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  absl::Cleanup destroy_actions = [&] {
    posix_spawn_file_actions_destroy(&actions);
  };
  posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, out_pipe[1]);
  posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, err_pipe[1]);

  std::vector<std::string> owned;
  owned.reserve(args.size() + 1);
  owned.push_back(bin.string());
  owned.insert(owned.end(), args.begin(), args.end());
  std::vector<char*> argv;
  argv.reserve(owned.size() + 1);
  for (std::string& s : owned) argv.push_back(s.data());
  argv.push_back(nullptr);

  pid_t pid = -1;
  const int rc =
      posix_spawn(&pid, bin.c_str(), &actions, nullptr, argv.data(), environ);
  close(out_pipe[1]);
  out_pipe[1] = -1;
  close(err_pipe[1]);
  err_pipe[1] = -1;
  if (rc != 0) {
    ADD_FAILURE() << "posix_spawn failed: " << std::strerror(rc);
    return res;
  }
  DrainPipesWithBudget(pid, out_pipe[0], err_pipe[0], res.stdout_text,
                       res.stderr_text, args);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (WIFEXITED(status)) {
    res.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    res.exit_code = 128 + WTERMSIG(status);
  }
  return res;
}

void WriteBytes(const std::filesystem::path& path,
                const std::vector<kdu_byte>& bytes) {
  std::ofstream ofs(path, std::ios::binary);
  ASSERT_TRUE(ofs.good()) << path;
  if (!bytes.empty()) {
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(ofs.good()) << path;
  }
}

void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
  std::ofstream ofs(path, std::ios::binary);
  ASSERT_TRUE(ofs.good()) << path;
  ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
  ASSERT_TRUE(ofs.good()) << path;
}

void AppendLe16(std::vector<kdu_byte>& out, kdu_uint16 v) {
  out.push_back(static_cast<kdu_byte>(v & 0xFF));
  out.push_back(static_cast<kdu_byte>((v >> 8) & 0xFF));
}

void AppendLe32(std::vector<kdu_byte>& out, kdu_uint32 v) {
  out.push_back(static_cast<kdu_byte>(v & 0xFF));
  out.push_back(static_cast<kdu_byte>((v >> 8) & 0xFF));
  out.push_back(static_cast<kdu_byte>((v >> 16) & 0xFF));
  out.push_back(static_cast<kdu_byte>((v >> 24) & 0xFF));
}

void WriteLe32At(std::vector<kdu_byte>& out, size_t pos, kdu_uint32 v) {
  out[pos + 0] = static_cast<kdu_byte>(v & 0xFF);
  out[pos + 1] = static_cast<kdu_byte>((v >> 8) & 0xFF);
  out[pos + 2] = static_cast<kdu_byte>((v >> 16) & 0xFF);
  out[pos + 3] = static_cast<kdu_byte>((v >> 24) & 0xFF);
}

void WritePgm8(const std::filesystem::path& path, int w, int h,
               bool split_mask = false) {
  const std::string hdr =
      "P5\n" + std::to_string(w) + " " + std::to_string(h) + "\n255\n";
  std::vector<kdu_byte> bytes(hdr.begin(), hdr.end());
  bytes.reserve(bytes.size() + static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (split_mask) {
        bytes.push_back((x < w / 2 && y < h / 2) ? 240 : 10);
      } else {
        bytes.push_back(static_cast<kdu_byte>((x * 19 + y * 37 + 11) & 0xFF));
      }
    }
  }
  WriteBytes(path, bytes);
}

void WritePgm16(const std::filesystem::path& path, int w, int h) {
  const std::string hdr =
      "P5\n" + std::to_string(w) + " " + std::to_string(h) + "\n65535\n";
  std::vector<kdu_byte> bytes(hdr.begin(), hdr.end());
  for (int i = 0; i < w * h; ++i) {
    const kdu_uint16 v = static_cast<kdu_uint16>((i * 4093 + 17) & 0xFFFF);
    bytes.push_back(static_cast<kdu_byte>((v >> 8) & 0xFF));
    bytes.push_back(static_cast<kdu_byte>(v & 0xFF));
  }
  WriteBytes(path, bytes);
}

void WritePpm8(const std::filesystem::path& path, int w, int h) {
  const std::string hdr =
      "P6\n" + std::to_string(w) + " " + std::to_string(h) + "\n255\n";
  std::vector<kdu_byte> bytes(hdr.begin(), hdr.end());
  bytes.reserve(bytes.size() + static_cast<size_t>(w) * h * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      bytes.push_back(static_cast<kdu_byte>((x * 29 + y * 13) & 0xFF));
      bytes.push_back(static_cast<kdu_byte>((x * 11 + y * 41 + 30) & 0xFF));
      bytes.push_back(static_cast<kdu_byte>((x * 17 + y * 23 + 90) & 0xFF));
    }
  }
  WriteBytes(path, bytes);
}

void WritePbm(const std::filesystem::path& path, int w, int h) {
  const std::string hdr =
      "P4\n" + std::to_string(w) + " " + std::to_string(h) + "\n";
  std::vector<kdu_byte> bytes(hdr.begin(), hdr.end());
  const int row_bytes = (w + 7) / 8;
  for (int y = 0; y < h; ++y) {
    for (int xb = 0; xb < row_bytes; ++xb) {
      bytes.push_back(static_cast<kdu_byte>((y * 53 + xb * 97 + 0x5A) & 0xFF));
    }
  }
  WriteBytes(path, bytes);
}

void WritePfmRgb(const std::filesystem::path& path, int w, int h) {
  const std::string hstr =
      "PF\n" + std::to_string(w) + " " + std::to_string(h) + "\n-1.0\n";
  std::vector<kdu_byte> bytes(hstr.begin(), hstr.end());
  for (int i = 0; i < w * h * 3; ++i) {
    const float v = -0.5f + static_cast<float>((i * 19) % 100) * 0.015f;
    AppendLe32(bytes, std::bit_cast<kdu_uint32>(v));
  }
  WriteBytes(path, bytes);
}

void WritePgxSigned16(const std::filesystem::path& path, int w, int h) {
  const std::string hdr =
      "PG ML -16 " + std::to_string(w) + " " + std::to_string(h) + "\n";
  std::vector<kdu_byte> bytes(hdr.begin(), hdr.end());
  for (int i = 0; i < w * h; ++i) {
    const auto v = static_cast<kdu_uint16>(
        static_cast<kdu_core::kdu_int16>((i * 313) % 4000 - 2000));
    bytes.push_back(static_cast<kdu_byte>((v >> 8) & 0xFF));
    bytes.push_back(static_cast<kdu_byte>(v & 0xFF));
  }
  WriteBytes(path, bytes);
}

void WriteBe32(std::vector<kdu_byte>& buf, size_t off, kdu_uint32 v) {
  buf[off + 0] = static_cast<kdu_byte>((v >> 24) & 0xFF);
  buf[off + 1] = static_cast<kdu_byte>((v >> 16) & 0xFF);
  buf[off + 2] = static_cast<kdu_byte>((v >> 8) & 0xFF);
  buf[off + 3] = static_cast<kdu_byte>(v & 0xFF);
}

std::vector<kdu_byte> BuildMinimalIcc(int num_colours) {
  if (num_colours != 3) {
    // 132-byte 'scnr' header for 1- or 4-color error testing.
    std::vector<kdu_byte> icc(132, 0);
    WriteBe32(icc, 0, 132);
    std::memcpy(icc.data() + 12, "scnr", 4);
    std::memcpy(icc.data() + 16, (num_colours == 4) ? "CMYK" : "GRAY", 4);
    std::memcpy(icc.data() + 20, "XYZ ", 4);
    return icc;
  }
  // 236-byte restricted 3-color 'mntr' ICC profile accepted by baseline JP2.
  constexpr size_t kCurvOff = 204;
  constexpr size_t kXyzOff = 216;
  constexpr size_t kTotalSize = 236;
  std::vector<kdu_byte> icc(kTotalSize, 0);
  WriteBe32(icc, 0, kTotalSize);
  std::memcpy(icc.data() + 12, "mntrRGB XYZ ", 12);
  WriteBe32(icc, 128, 6);  // tag count
  constexpr auto kSigs = std::to_array<const char*>({
      "rTRC",
      "gTRC",
      "bTRC",
      "rXYZ",
      "gXYZ",
      "bXYZ",
  });
  for (size_t i = 0; i < kSigs.size(); ++i) {
    const size_t rec = 132 + i * 12;
    std::memcpy(icc.data() + rec, kSigs[i], 4);
    WriteBe32(icc, rec + 4,
              static_cast<kdu_uint32>(i < 3 ? kCurvOff : kXyzOff));
    WriteBe32(icc, rec + 8, i < 3 ? 12U : 20U);
  }
  std::memcpy(icc.data() + kCurvOff, "curv", 4);
  std::memcpy(icc.data() + kXyzOff, "XYZ ", 4);
  WriteBe32(icc, kXyzOff + 8, 0x00010000U);  // X = 1.0 in s15Fixed16Number
  return icc;
}

void WriteBmp24BottomUp(const std::filesystem::path& path, int w, int h) {
  const int stride = ((w * 3 + 3) / 4) * 4;
  std::vector<kdu_byte> px(static_cast<size_t>(stride) * h, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t off = static_cast<size_t>(y) * stride + x * 3;
      px[off + 0] = static_cast<kdu_byte>((x * 21 + y * 13) & 0xFF);
      px[off + 1] = static_cast<kdu_byte>((x * 17 + y * 29) & 0xFF);
      px[off + 2] = static_cast<kdu_byte>((x * 31 + y * 7) & 0xFF);
    }
  }
  std::vector<kdu_byte> out;
  constexpr kdu_uint32 kOff = 54;
  out.push_back('B');
  out.push_back('M');
  AppendLe32(out, kOff + static_cast<kdu_uint32>(px.size()));
  AppendLe16(out, 0);
  AppendLe16(out, 0);
  AppendLe32(out, kOff);
  AppendLe32(out, 40);
  AppendLe32(out, static_cast<kdu_uint32>(w));
  AppendLe32(out, static_cast<kdu_uint32>(h));  // positive => bottom-up
  AppendLe16(out, 1);
  AppendLe16(out, 24);
  AppendLe32(out, 0);
  AppendLe32(out, static_cast<kdu_uint32>(px.size()));
  AppendLe32(out, 2835);
  AppendLe32(out, 2835);
  AppendLe32(out, 0);
  AppendLe32(out, 0);
  out.insert(out.end(), px.begin(), px.end());
  WriteBytes(path, out);
}

void WriteBmp8Palettized(const std::filesystem::path& path, int w, int h,
                         bool monochrome) {
  const int stride = ((w + 3) / 4) * 4;
  std::vector<kdu_byte> pal(256 * 4, 0);
  for (int i = 0; i < 16; ++i) {
    const kdu_byte v = static_cast<kdu_byte>(i * 16);
    pal[i * 4 + 0] = monochrome ? v : static_cast<kdu_byte>(255 - v);
    pal[i * 4 + 1] = v;
    pal[i * 4 + 2] = monochrome ? v : static_cast<kdu_byte>((v + 64) & 0xFF);
  }
  std::vector<kdu_byte> px(static_cast<size_t>(stride) * h, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      px[static_cast<size_t>(y) * stride + x] =
          static_cast<kdu_byte>((x + y * 3) & 0x0F);
    }
  }
  std::vector<kdu_byte> out;
  const kdu_uint32 off = 54 + static_cast<kdu_uint32>(pal.size());
  out.push_back('B');
  out.push_back('M');
  AppendLe32(out, off + static_cast<kdu_uint32>(px.size()));
  AppendLe16(out, 0);
  AppendLe16(out, 0);
  AppendLe32(out, off);
  AppendLe32(out, 40);
  AppendLe32(out, static_cast<kdu_uint32>(w));
  AppendLe32(out, static_cast<kdu_uint32>(-h));  // top-down
  AppendLe16(out, 1);
  AppendLe16(out, 8);
  AppendLe32(out, 0);
  AppendLe32(out, static_cast<kdu_uint32>(px.size()));
  AppendLe32(out, 2835);
  AppendLe32(out, 2835);
  AppendLe32(out, 16);
  AppendLe32(out, 16);
  out.insert(out.end(), pal.begin(), pal.end());
  out.insert(out.end(), px.begin(), px.end());
  WriteBytes(path, out);
}

// Writes an uncompressed little-endian RGB or RGBA TIFF with resolution,
// optional premultiplied alpha, XMP metadata (tag 700), and optional ICC
// profile (tag 34675).
void WriteRgbaMetaTiff(const std::filesystem::path& path, int w, int h,
                       bool include_icc, int icc_colours = 3,
                       bool include_alpha = true) {
  struct TagSpec {
    kdu_uint16 tag;
    kdu_uint16 type;
    kdu_uint32 count;
    std::vector<kdu_byte> data;
  };
  auto short_tag = [](kdu_uint16 tag, std::initializer_list<kdu_uint16> vals) {
    TagSpec s{tag, 3, static_cast<kdu_uint32>(vals.size()), {}};
    for (kdu_uint16 v : vals) AppendLe16(s.data, v);
    return s;
  };
  auto long_tag = [](kdu_uint16 tag, std::initializer_list<kdu_uint32> vals) {
    TagSpec s{tag, 4, static_cast<kdu_uint32>(vals.size()), {}};
    for (kdu_uint32 v : vals) AppendLe32(s.data, v);
    return s;
  };
  auto rational_tag = [](kdu_uint16 tag, kdu_uint32 num, kdu_uint32 den) {
    TagSpec s{tag, 5, 1, {}};
    AppendLe32(s.data, num);
    AppendLe32(s.data, den);
    return s;
  };

  const int spp = include_alpha ? 4 : 3;
  std::vector<kdu_byte> xmp = {'<', 'x', '/', '>', '\n'};
  std::vector<TagSpec> tags = {
      short_tag(256, {static_cast<kdu_uint16>(w)}),
      short_tag(257, {static_cast<kdu_uint16>(h)}),
      include_alpha ? short_tag(258, {8, 8, 8, 8}) : short_tag(258, {8, 8, 8}),
      short_tag(259, {1}),
      short_tag(262, {2}),
      long_tag(273, {0}),
      short_tag(277, {static_cast<kdu_uint16>(spp)}),
      short_tag(278, {static_cast<kdu_uint16>(h)}),
      long_tag(279, {static_cast<kdu_uint32>(w * h * spp)}),
      rational_tag(282, 72, 1),
      rational_tag(283, 72, 1),
      short_tag(284, {1}),
      short_tag(296, {2}),
  };
  if (include_alpha) {
    tags.push_back(short_tag(338, {1}));
  }
  tags.push_back(TagSpec{700, 1, static_cast<kdu_uint32>(xmp.size()), xmp});
  if (include_icc) {
    std::vector<kdu_byte> icc = BuildMinimalIcc(icc_colours);
    tags.push_back(TagSpec{34675, 7, static_cast<kdu_uint32>(icc.size()), icc});
  }

  std::vector<kdu_byte> out = {'I', 'I', 42, 0, 8, 0, 0, 0};
  AppendLe16(out, static_cast<kdu_uint16>(tags.size()));
  const size_t entries_start = out.size();
  out.resize(entries_start + tags.size() * 12 + 4, 0);

  size_t strip_off_val_pos = 0;
  for (size_t i = 0; i < tags.size(); ++i) {
    const size_t epos = entries_start + i * 12;
    out[epos + 0] = static_cast<kdu_byte>(tags[i].tag & 0xFF);
    out[epos + 1] = static_cast<kdu_byte>((tags[i].tag >> 8) & 0xFF);
    out[epos + 2] = static_cast<kdu_byte>(tags[i].type & 0xFF);
    out[epos + 3] = static_cast<kdu_byte>((tags[i].type >> 8) & 0xFF);
    WriteLe32At(out, epos + 4, tags[i].count);
    if (tags[i].tag == 273) strip_off_val_pos = epos + 8;
    if (tags[i].data.size() <= 4) {
      std::memcpy(out.data() + epos + 8, tags[i].data.data(),
                  tags[i].data.size());
    } else {
      const auto data_off = static_cast<kdu_uint32>(out.size());
      WriteLe32At(out, epos + 8, data_off);
      out.insert(out.end(), tags[i].data.begin(), tags[i].data.end());
      if ((out.size() & 1U) != 0) out.push_back(0);
    }
  }
  const auto pixel_off = static_cast<kdu_uint32>(out.size());
  WriteLe32At(out, strip_off_val_pos, pixel_off);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      out.push_back(static_cast<kdu_byte>((x * 19 + y * 7) & 0xFF));
      out.push_back(static_cast<kdu_byte>((x * 13 + y * 23) & 0xFF));
      out.push_back(static_cast<kdu_byte>((x * 31 + y * 11) & 0xFF));
      if (include_alpha) {
        out.push_back(static_cast<kdu_byte>(200 + ((x + y) & 0x1F)));
      }
    }
  }
  WriteBytes(path, out);
}

void ExpectNonEmptyFile(const std::filesystem::path& path) {
  std::error_code ec;
  ASSERT_TRUE(std::filesystem::exists(path, ec)) << path;
  EXPECT_GT(std::filesystem::file_size(path, ec), 0U) << path;
}

TEST(KduCompressCcTest, HelpVersionAndArgumentErrors) {
  const std::filesystem::path bin =
      FindBinary("third_party/kakadu/vendor/kdu_compress");
  ASSERT_TRUE(std::filesystem::exists(bin)) << bin;
  ScopedTempDir tmp("compress_args");

  const std::filesystem::path gray = tmp.File("gray.pgm");
  WritePgm8(gray, 16, 16);
  const std::filesystem::path odd_rgb = tmp.File("odd_rgb.ppm");
  WritePpm8(odd_rgb, 7, 8);
  const std::filesystem::path rgb = tmp.File("rgb.ppm");
  WritePpm8(rgb, 16, 16);
  const std::filesystem::path bmp_bu = tmp.File("bottomup.bmp");
  WriteBmp24BottomUp(bmp_bu, 16, 16);
  const std::filesystem::path bmp_pal = tmp.File("pal.bmp");
  WriteBmp8Palettized(bmp_pal, 16, 16, false);
  const std::filesystem::path tif_bad_icc = tmp.File("bad_icc.tif");
  WriteRgbaMetaTiff(tif_bad_icc, 8, 8, /*include_icc=*/true,
                    /*icc_colours=*/4, /*include_alpha=*/false);
  const std::filesystem::path raw16 = tmp.File("raw16.raw");
  WriteBytes(raw16, std::vector<kdu_byte>(16 * 16, 0x42));
  const std::filesystem::path out_j2k = tmp.File("err_out.j2k");
  const std::filesystem::path out_jp2 = tmp.File("err_out.jp2");
  const std::filesystem::path out_jpx = tmp.File("err_out.jpx");

  // Help and version statements.
  EXPECT_EQ(RunTool(bin, {}).exit_code, 0);
  EXPECT_EQ(RunTool(bin, {"-u"}).exit_code, 0);
  const RunResult usage = RunTool(bin, {"-usage"});
  EXPECT_EQ(usage.exit_code, 0);
  EXPECT_THAT(usage.stdout_text, ::testing::HasSubstr("-bstats"));
  const RunResult ver = RunTool(bin, {"-v"});
  EXPECT_EQ(ver.exit_code, 0);
  EXPECT_THAT(ver.stdout_text, ::testing::HasSubstr("kdu_compress"));
  EXPECT_EQ(RunTool(bin, {"-version"}).exit_code, 0);

  const std::vector<std::vector<std::string>> bad_invocations = {
      {"-num_threads", "0"},
      {"-i"},
      {"-i", gray.string() + "*0@64", "-num_threads", "0"},
      {"-i", gray.string() + "*2", "-num_threads", "0"},
      {"-i", gray.string(), "-icrop"},
      // Empty "-icrop" breaks cleanly before vendor kdu_compress.cpp:1406 NULL
      // `file` dereference (omitting `-i` or excess `-icrop` entries
      // segfaults).
      {"-i", gray.string(), "-icrop", "", "-num_threads", "0"},
      {"-i", gray.string(), "-icrop", "{0,0,0,4}", "-num_threads", "0"},
      {"-i", gray.string() + "," + gray.string(), "-icrop",
       "{0,0,4,4}{0,0,4,4}", "-num_threads", "0"},
      {"-i", raw16.string(), "-icrop", "{0,0,8,8}", "Sdims={16,16}",
       "Nprecision=8", "Nsigned=no", "-num_threads", "0"},
      {"-i", bmp_bu.string() + "," + gray.string(), "-num_threads", "0"},
      {"-i", gray.string(), "-o"},
      {"-i", gray.string(), "-rotate"},
      {"-i", gray.string(), "-rotate", "45"},
      {"-i", gray.string(), "-num_threads"},
      {"-i", gray.string(), "-num_threads", "-1"},
      {"-i", gray.string(), "-num_threads", "2x"},
      {"-i", gray.string(), "-num_threads", "2,1X"},
      {"-i", gray.string(), "-periodic_trimming"},
      {"-i", gray.string(), "-periodic_trimming", "maybe"},
      {"-i", gray.string(), "-double_buffering"},
      {"-i", gray.string(), "-double_buffering", "-1"},
      {"-i", gray.string(), "-split"},
      {"-i", gray.string(), "-split", "16,2"},
      {"-i", gray.string(), "-cpu"},
      {"-i", gray.string(), "-cpu", "-1"},
      {"-i", gray.string(), "-tolerance"},
      {"-i", gray.string(), "-tolerance", "60"},
      {"-i", gray.string(), "-flush_period"},
      {"-i", gray.string(), "-flush_period", "0"},
      {"-i", gray.string(), "-no_weights", "-grey_weights"},
      {"-i", gray.string(), "-grey_weights", "-chroma_weights", "1"},
      {"-i", gray.string(), "-no_weights", "-chroma_weights", "1"},
      {"-i", gray.string(), "-chroma_weights", "4"},
      {"-i", rgb.string(), "-chroma_weights", "1", "Cycc=yes", "-num_threads",
       "0"},
      {"-i", gray.string(), "-jpx_layers"},
      {"-i", gray.string(), "-jpx_layers", "0"},
      {"-i", gray.string(), "-progress"},
      {"-i", gray.string(), "-progress", "0"},
      {"-i", gray.string(), "-record"},
      {"-i", gray.string(), "-record", "/nonexistent_kdu_dir/rec.txt"},
      {"-i", gray.string(), "-num_threads", "0", "-fprec"},
      {"-i", gray.string(), "-num_threads", "0", "-fprec", "40"},
      {"-i", gray.string(), "-num_threads", "0", "-fprec", "16F16"},
      {"-i", gray.string(), "-num_threads", "0", "-fprec", "16F5",
       "NLType=SMAG"},
      {"-i", gray.string(), "-num_threads", "0", "-fprec", "16F5",
       "NLType=NONE"},
      {"-i", gray.string(), "-o", out_j2k.string(), "-num_threads", "0",
       "-frag", "0,0,1"},
      {"-i", gray.string(), "-num_threads", "0", "-frag", "0,0,1,1",
       "Sdims={16,16}", "Stiles={16,16}"},
      {"-i", gray.string(), "-o", out_j2k.string(), "-num_threads", "0",
       "-frag", "5,5,1,1", "Sdims={16,16}", "Stiles={16,16}"},
      {"-i", gray.string(), "-o", tmp.File("no_prior.j2k").string(),
       "-num_threads", "0", "-frag", "0,1,1,2", "Sdims={16,32}",
       "Stiles={16,16}"},
      {"-i", gray.string(), "-o", out_j2k.string(), "-num_threads", "0",
       "-frag", "0,0,1,2", "Sdims={16,32}", "Stiles={16,16}", "-rotate", "90"},
      {"-i", gray.string(), "-num_threads", "0", "-rate"},
      {"-i", gray.string(), "-num_threads", "0", "-rate", "0"},
      {"-i", gray.string(), "-num_threads", "0", "Clayers=1", "-rate",
       "1.0,2.0"},
      {"-i", gray.string(), "-num_threads", "0", "-slope"},
      {"-i", gray.string(), "-num_threads", "0", "-slope", "70000"},
      {"-i", gray.string(), "-num_threads", "0", "Clevels=1", "Corder=PCRL",
       "Cprecincts={8,16},{4,16}", "Cblk={4,4}", "Scbr={1,16}"},
      {"-i", gray.string(), "-num_threads", "0", "-com"},
      {"-i", gray.string(), "-num_threads", "0", "-roi", "{-0.1,0.0},{0.5,0.5}",
       "Rshift=4"},
      {"-i", gray.string(), "-num_threads", "0", "-roi", gray.string() + ",1.0",
       "Rshift=4"},
      {"-i", gray.string(), "-num_threads", "0", "-bstats"},
      {"-i", gray.string(), "-num_threads", "0", "-bstats",
       tmp.File("missing_bstats.txt").string()},
      {"-i", gray.string(), "-num_threads", "0", "-bogus_flag"},
      {"-i", gray.string(), "-num_threads", "0", "-rgb_to_420"},
      {"-i", odd_rgb.string(), "-num_threads", "0", "-rgb_to_420",
       "Creversible=no"},
      {"-i", rgb.string(), "-num_threads", "0", "-rgb_to_420",
       "Creversible=yes"},
      {"-i", rgb.string(), "-num_threads", "0", "-rgb_to_420", "Creversible=no",
       "Stiles={8,8}"},
      {"-i", rgb.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jp2_aspect", "0"},
      {"-i", gray.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jph_non_space"},
      {"-i", gray.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jph_non_space", "ZZ"},
      {"-i", gray.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jph_non_space", "524C,474C"},
      {"-i", bmp_pal.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jph_non_space", "524C"},
      {"-i", gray.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jp2_space"},
      {"-i", gray.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jp2_space", "bogus"},
      {"-i", gray.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jp2_space", "iccLUM,0.5,0.1"},
      {"-i", rgb.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jp2_space", "iccRGB,1,2"},
      {"-i", rgb.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jp2_space", "iccRGB,2.2,0.05,0.64,0.33,0.30,0.60,0.15,0.06,D75"},
      {"-i", rgb.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jp2_space", "iccRGB,2.2,0.05,1.5,0.33,0.30,0.60,0.15,0.06,D65"},
      {"-i", gray.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jp2_space", "sRGB"},
      {"-i", gray.string(), "-o", out_jpx.string(), "-num_threads", "0",
       "-jpx_space"},
      {"-i", gray.string(), "-o", out_jpx.string(), "-num_threads", "0",
       "-jpx_space", "bogus_space"},
      {"-i", gray.string(), "-o", out_jpx.string(), "-num_threads", "0",
       "-jpx_space", "sLUM,200,0"},
      {"-i", rgb.string(), "-o", out_jpx.string(), "-num_threads", "0",
       "-jp2_space", "sYCC", "-jpx_space", "sRGB"},
      {"-i", gray.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jp2_alpha"},
      {"-i", rgb.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jp2_alpha"},
      {"-i", tif_bad_icc.string(), "-o", out_jp2.string(), "-num_threads", "0"},
      {"-i", gray.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jp2_box"},
      {"-i", gray.string(), "-o", out_jp2.string(), "-num_threads", "0",
       "-jp2_box", tmp.File("no_box.bin").string()},
  };
  for (size_t i = 0; i < bad_invocations.size(); ++i) {
    const RunResult res = RunTool(bin, bad_invocations[i]);
    EXPECT_NE(res.exit_code, 0)
        << "Case " << i << ": " << JoinArgs(bad_invocations[i]);
  }
}

TEST(KduCompressCcTest, FormatsRatesSlopesRoiWeightsAnd420) {
  const std::filesystem::path bin =
      FindBinary("third_party/kakadu/vendor/kdu_compress");
  ASSERT_TRUE(std::filesystem::exists(bin)) << bin;
  ScopedTempDir tmp("compress_formats");
  auto expect_ok = [&](const std::vector<std::string>& args) -> RunResult {
    RunResult res = RunTool(bin, args);
    EXPECT_EQ(res.exit_code, 0) << res.stderr_text;
    return res;
  };

  const std::filesystem::path gray8 = tmp.File("gray8.pgm");
  WritePgm8(gray8, 16, 16);
  const std::filesystem::path gray16 = tmp.File("gray16.pgm");
  WritePgm16(gray16, 16, 16);
  const std::filesystem::path mask = tmp.File("mask.pgm");
  WritePgm8(mask, 8, 8, /*split_mask=*/true);
  const std::filesystem::path rgb = tmp.File("rgb.ppm");
  WritePpm8(rgb, 16, 16);
  const std::filesystem::path pbm = tmp.File("bits.pbm");
  WritePbm(pbm, 16, 16);
  const std::filesystem::path pfm = tmp.File("float.pfm");
  WritePfmRgb(pfm, 8, 8);
  const std::filesystem::path pgx = tmp.File("signed16.pgx");
  WritePgxSigned16(pgx, 8, 8);
  const std::filesystem::path bmp24 = tmp.File("bu24.bmp");
  WriteBmp24BottomUp(bmp24, 16, 16);
  const std::filesystem::path tif_rgba = tmp.File("rgba_crop.tif");
  WriteRgbaMetaTiff(tif_rgba, 16, 16, /*include_icc=*/true,
                    /*icc_colours=*/3);
  const std::filesystem::path tif_rgb_icc = tmp.File("rgb_icc.tif");
  WriteRgbaMetaTiff(tif_rgb_icc, 16, 16, /*include_icc=*/true,
                    /*icc_colours=*/3, /*include_alpha=*/false);

  std::vector<kdu_byte> raw_buf(64 * 3);
  for (size_t i = 0; i < raw_buf.size(); ++i) {
    raw_buf[i] = static_cast<kdu_byte>((i * 17 + 3) & 0xFF);
  }
  const std::filesystem::path raw = tmp.File("multi.raw");
  WriteBytes(raw, raw_buf);
  const std::filesystem::path rawl = tmp.File("comp.rawl");
  WriteBytes(rawl, std::vector<kdu_byte>(16 * 16, 0x3C));

  // 1. Input format coverage + Qstep high-bit-depth warning + structured null
  // target + switch/record/diagnostic flags.
  expect_ok({"-i",
             gray8.string(),
             "-o",
             tmp.File("g8.j2k").string(),
             "-num_threads",
             "0",
             "-double_buffering",
             "4",
             "-progress",
             "4",
             "-mem",
             "-mem_limit",
             "8192",
             "-cpu",
             "0",
             "-bc_noopt",
             "-no_info",
             "-no_version",
             "-com",
             "hello_com",
             "-com",
             "c2",
             "-split",
             "0,0,1"});
  expect_ok({"-i", gray16.string(), "-o", tmp.File("g16.j2k").string(),
             "-num_threads", "0", "Creversible=no"});
  expect_ok({"-i", rgb.string(), "-num_threads", "0", "-flush_period", "3",
             "-mem_limit", "-cpu", "2", "Stiles={8,8}"});
  expect_ok({"-i", pbm.string(), "-o", tmp.File("pbm.j2k").string(),
             "-num_threads", "0"});
  expect_ok({"-i", pfm.string(), "-o", tmp.File("pfm.j2k").string(),
             "-num_threads", "0", "-fprec", "32F8"});
  expect_ok({"-i", pgx.string(), "-o", tmp.File("pgx.j2k").string(),
             "-num_threads", "0"});
  expect_ok({"-i", bmp24.string(), "-o", tmp.File("bmp24.j2k").string(),
             "-num_threads", "0"});
  expect_ok({"-i", raw.string() + "*3@64", "-o", tmp.File("raw3.j2k").string(),
             "-num_threads", "0", "Sdims={8,8}", "Nprecision=8", "Nsigned=no"});
  // Mixed files triggering append_component() + per-component SMAG/UMAG NLType.
  expect_ok({"-i", rawl.string() + "," + gray8.string(), "-o",
             tmp.File("rawl_mix.j2k").string(), "-num_threads", "0",
             "Sdims={16,16}", "Nprecision=8", "Nsigned=no", "-fprec", "16F5"});
  expect_ok({"-i", mask.string() + "," + pgx.string(), "-o",
             tmp.File("mix_fprec.j2k").string(), "-num_threads", "0", "-fprec",
             "16F5"});
  // Uncompressed TIFF cropping with -icrop.
  expect_ok({"-i", tif_rgba.string(), "-icrop", "{2,2,8,8}", "-o",
             tmp.File("tif_crop.j2k").string(), "-num_threads", "0"});

  const std::filesystem::path switch_file = tmp.File("switches.txt");
  const std::filesystem::path rec_file = tmp.File("record.txt");
  const std::filesystem::path sw_out = tmp.File("from_switch.j2k");
  WriteTextFile(switch_file, "# comment\n% percent comment\n-quiet\n-o\n" +
                                 sw_out.string() + "\n");
  expect_ok({"-i", gray8.string(), "-s", switch_file.string(), "-record",
             rec_file.string(), "-num_threads", "0"});
  ExpectNonEmptyFile(sw_out);
  ExpectNonEmptyFile(rec_file);

  for (int angle : {0, 90, 180, 270, -90, 450}) {
    const std::filesystem::path out =
        tmp.File("rot_" + std::to_string(angle) + ".j2k");
    expect_ok({"-i", rgb.string(), "-o", out.string(), "-num_threads", "0",
               "-rotate", std::to_string(angle), "-flip", "-precise"});
    ExpectNonEmptyFile(out);
  }
  expect_ok({"-i", rgb.string(), "-o", tmp.File("vp.j2k").string(),
             "-num_threads", "0", "-vprecise"});

  // 2. Rate control, slopes (sort, interpolate, extrapolate, clamp), CBR,
  // bstats.
  expect_ok({"-i", rgb.string(), "-o", tmp.File("rates_dash.j2k").string(),
             "-num_threads", "0", "-rate", "-,2.0,0.5", "-tolerance", "0",
             "-trim_to_rate", "-periodic_trimming", "yes"});
  expect_ok({"-i", rgb.string(), "-o", tmp.File("rates_bounds.j2k").string(),
             "-num_threads", "0", "Clayers=3", "-rate", "2.0,0.5",
             "-periodic_trimming", "no", "-full"});
  expect_ok({"-i", rgb.string(), "-o", tmp.File("slope1.j2k").string(),
             "-num_threads", "0", "-slope", "42000"});
  expect_ok({"-i", rgb.string(), "-o", tmp.File("slope_interp.j2k").string(),
             "-num_threads", "0", "Clayers=4", "-slope", "44000,45000"});
  expect_ok({"-i", rgb.string(), "-o", tmp.File("slope_interp2.j2k").string(),
             "-num_threads", "0", "Clayers=4", "-slope", "45000,44300"});
  expect_ok({"-i", rgb.string(), "-o", tmp.File("slope_extrap.j2k").string(),
             "-num_threads", "0", "Clayers=4", "-slope", "45000,44900"});
  expect_ok({"-i", rgb.string(), "-o",
             tmp.File("slope_extrap_big.j2k").string(), "-num_threads", "0",
             "Clayers=10", "-slope", "45000,44400"});
  expect_ok({"-i", rgb.string(), "-o", tmp.File("slope_clamp.j2k").string(),
             "-num_threads", "0", "Clayers=4", "-slope", "65400,65300"});

  {
    const RunResult cbr =
        RunTool(bin, {"-i", gray8.string(), "-o", tmp.File("cbr.j2k").string(),
                      "-num_threads", "0", "Creversible=no", "Clevels=1",
                      "Corder=PCRL", "Cprecincts={8,16},{4,16}", "Cblk={4,4}",
                      "Scbr={1,16}", "-rate", "16.0"});
    EXPECT_EQ(cbr.exit_code, 0) << cbr.stderr_text;
    EXPECT_THAT(cbr.stdout_text, ::testing::HasSubstr("CBR Latency Analysis"));
  }

  const std::filesystem::path bst1 = tmp.File("bst1.txt");
  const std::filesystem::path bst2 = tmp.File("bst2.txt");
  expect_ok({"-i", rgb.string(), "-o", tmp.File("bst1.j2k").string(),
             "-num_threads", "0", "Creversible=no", "-rate", "2.0", "-bstats",
             "-," + bst1.string()});
  ExpectNonEmptyFile(bst1);
  // Load bst1 on mismatched component count + different Qstep to exercise
  // warning tallies and saving updated statistics to bst2.
  expect_ok({"-i", gray8.string(), "-o", tmp.File("bst2.j2k").string(),
             "-num_threads", "0", "Creversible=no", "Qstep=0.05", "-rate",
             "2.0", "-bstats", bst1.string() + "," + bst2.string()});
  // Cplex-EST recommendations (with & without Scbr) and non-EST warnings.
  expect_ok({"-i", gray8.string(), "-o", tmp.File("cplex_est.j2k").string(),
             "-num_threads", "0", "Creversible=no", "Cplex={6,EST,0.25,0}",
             "-rate", "2.0"});
  expect_ok({"-i", gray8.string(), "-o", tmp.File("cplex_cbr.j2k").string(),
             "-num_threads", "0", "Creversible=no", "Clevels=1", "Corder=PCRL",
             "Cprecincts={8,16},{4,16}", "Cblk={4,4}", "Scbr={1,16}",
             "Cplex={6,EST,0.25,0}", "-rate", "16.0"});
  expect_ok({"-i", gray8.string(), "-o", tmp.File("cplex_fix.j2k").string(),
             "-num_threads", "0", "Creversible=no", "Cplex={6,FIX,0.25,0}",
             "-bstats", "-," + tmp.File("bst_unused.txt").string(), "-rate",
             "2.0"});
  expect_ok({"-i", gray8.string(), "-o", tmp.File("bstats_norate.j2k").string(),
             "-num_threads", "0", "Creversible=no", "-bstats",
             "-," + tmp.File("bst_norate.txt").string()});

  // 3. Visual weighting policies, ROI (rect & PGM mask), and -rgb_to_420.
  expect_ok({"-i", rgb.string(), "-o", tmp.File("w_none.j2k").string(),
             "-num_threads", "0", "-no_weights", "-rate", "1.5"});
  expect_ok({"-i", gray8.string(), "-o", tmp.File("w_grey.j2k").string(),
             "-num_threads", "0", "-grey_weights", "-rate", "1.5"});
  expect_ok({"-i", gray8.string(), "-o", tmp.File("w_qf.j2k").string(),
             "-num_threads", "0", "Creversible=no", "Qfactor=80"});
  expect_ok({"-i", rgb.string(), "-o", tmp.File("w_explicit.j2k").string(),
             "-num_threads", "0", "Clev_weights={1.0}", "-no_weights"});
  constexpr auto kChromaWeights =
      std::to_array<std::string_view>({"1", "2", "3"});
  for (std::string_view cw : kChromaWeights) {
    expect_ok({"-i", rgb.string(), "-o",
               tmp.File("cw_" + std::string(cw) + ".j2k").string(),
               "-num_threads", "0", "-chroma_weights", std::string(cw),
               "Cycc=no"});
  }
  expect_ok({"-i", gray8.string(), "-o", tmp.File("roi_rect.j2k").string(),
             "-num_threads", "0", "-roi", "{0.1,0.1},{0.5,0.5}", "Rshift=16",
             "-split", "16,2,2"});
  expect_ok({"-i", rgb.string(), "-o", tmp.File("roi_mask.j2k").string(),
             "-num_threads", "0", "-roi", mask.string() + ",0.5", "Rshift=16"});

  const std::filesystem::path j2k_420_16 = tmp.File("out420_16.j2k");
  const std::filesystem::path jp2_420_32 = tmp.File("out420_32.jp2");
  {
    const RunResult res420 =
        RunTool(bin, {"-i", rgb.string(), "-o", j2k_420_16.string(),
                      "-num_threads", "0", "-rgb_to_420", "-chroma_weights",
                      "2", "Creversible=no", "CRGoffset={0,0}"});
    ASSERT_EQ(res420.exit_code, 0) << res420.stderr_text;
    ExpectNonEmptyFile(j2k_420_16);
  }
  expect_ok({"-i", tif_rgb_icc.string(), "-o", jp2_420_32.string(),
             "-num_threads", "0", "-rgb_to_420", "Creversible=no", "-precise"});

  kdu_simple_file_source src420;
  ASSERT_TRUE(src420.open(j2k_420_16.c_str(), false, true));
  kdu_codestream cs420;
  absl::Cleanup destroy_cs420 = [&] {
    if (cs420.exists()) cs420.destroy();
  };
  cs420.create(&src420);
  EXPECT_EQ(cs420.get_num_components(), 3);
  kdu_dims d0, d1, d2;
  cs420.get_dims(0, d0);
  cs420.get_dims(1, d1);
  cs420.get_dims(2, d2);
  EXPECT_EQ(d0.size.x, 16);
  EXPECT_EQ(d0.size.y, 16);
  EXPECT_EQ(d1.size.x, 8);
  EXPECT_EQ(d1.size.y, 8);
  EXPECT_EQ(d2.size.x, 8);
  EXPECT_EQ(d2.size.y, 8);
}

TEST(KduCompressCcTest, MultiThreadingFragmentsJp2AndJpxContainers) {
  const std::filesystem::path bin =
      FindBinary("third_party/kakadu/vendor/kdu_compress");
  ASSERT_TRUE(std::filesystem::exists(bin)) << bin;
  ScopedTempDir tmp("compress_mt_jp2");
  auto expect_ok = [&](const std::vector<std::string>& args) -> RunResult {
    RunResult res = RunTool(bin, args);
    EXPECT_EQ(res.exit_code, 0) << res.stderr_text;
    return res;
  };

  const std::filesystem::path rgb = tmp.File("rgb16.ppm");
  WritePpm8(rgb, 16, 16);
  const std::filesystem::path gray = tmp.File("gray16.pgm");
  WritePgm8(gray, 16, 16);
  const std::filesystem::path tile_top = tmp.File("top8x16.pgm");
  WritePgm8(tile_top, 16, 8);
  const std::filesystem::path tile_bot = tmp.File("bot8x16.pgm");
  WritePgm8(tile_bot, 16, 8);
  const std::filesystem::path bmp_pal = tmp.File("pal8.bmp");
  WriteBmp8Palettized(bmp_pal, 16, 16, /*monochrome=*/false);
  const std::filesystem::path bmp_mono = tmp.File("mono8.bmp");
  WriteBmp8Palettized(bmp_mono, 16, 16, /*monochrome=*/true);
  const std::filesystem::path tif_rgba = tmp.File("rgba.tif");
  WriteRgbaMetaTiff(tif_rgba, 16, 16, /*include_icc=*/true,
                    /*icc_colours=*/3);

  // 1. Multi-threading modes, affinity bundles, double buffering, auto_flush.
  constexpr auto kThreadModes = std::to_array<std::string_view>(
      {"2", "U2", "3,1T,1C", "U3,1T,1C", "1,2T,2C"});
  int mt_idx = 0;
  for (std::string_view nt : kThreadModes) {
    const std::filesystem::path out =
        tmp.File("mt_" + std::to_string(mt_idx++) + ".j2k");
    expect_ok({"-i", rgb.string(), "-o", out.string(), "-num_threads",
               std::string(nt), "-double_buffering", "4", "-split", "8,2,2",
               "-progress", "2", "-mem", "-cpu", "1", "-flush_period", "2",
               "Stiles={8,8}"});
    ExpectNonEmptyFile(out);
  }
  expect_ok({"-i", rgb.string(), "-o", tmp.File("mt_flush_tc.j2k").string(),
             "-num_threads", "2", "-flush_period", "12", "Stiles={8,8}"});
  if (HostFitsMaxThreads()) {
    expect_ok({"-i", gray.string(), "-o", tmp.File("mt_default.j2k").string()});
  }

  // 2. Fragmented compression across multiple invocations (-frag).
  const std::filesystem::path frag_j2k = tmp.File("frag.j2k");
  {
    const RunResult f0 = expect_ok(
        {"-i", tile_top.string(), "-o", frag_j2k.string(), "-num_threads", "0",
         "-frag", "0,0,1,2", "Sdims={16,16}", "Stiles={8,8}", "ORGgen_tlm=2"});
    EXPECT_THAT(f0.stdout_text, ::testing::HasSubstr("more fragments"));

    const std::filesystem::path frag_bad_tlm = tmp.File("frag_bad_tlm.j2k");
    std::filesystem::copy_file(frag_j2k, frag_bad_tlm);
    const RunResult bad =
        RunTool(bin, {"-i", tile_bot.string(), "-o", frag_bad_tlm.string(),
                      "-num_threads", "0", "-frag", "1,0,1,2", "Sdims={16,16}",
                      "Stiles={8,8}", "ORGgen_tlm=1"});
    EXPECT_NE(bad.exit_code, 0);

    const RunResult f1 =
        RunTool(bin, {"-i", tile_bot.string(), "-o", frag_j2k.string(),
                      "-num_threads", "0", "-frag", "1,0,1,2", "Sdims={16,16}",
                      "Stiles={8,8}", "ORGgen_tlm=2"});
    ASSERT_EQ(f1.exit_code, 0) << f1.stderr_text;
    EXPECT_THAT(f1.stdout_text,
                ::testing::HasSubstr("All fragments successfully compressed"));
  }
  ExpectNonEmptyFile(frag_j2k);
  {
    kdu_simple_file_source src;
    ASSERT_TRUE(src.open(frag_j2k.c_str(), false, true));
    kdu_codestream cs;
    absl::Cleanup destroy_cs = [&] {
      if (cs.exists()) cs.destroy();
    };
    cs.create(&src);
    kdu_dims canvas;
    cs.get_dims(-1, canvas);
    EXPECT_EQ(canvas.size.x, 16);
    EXPECT_EQ(canvas.size.y, 16);
    kdu_dims tiles;
    cs.get_valid_tiles(tiles);
    EXPECT_EQ(tiles.size.x, 2);
    EXPECT_EQ(tiles.size.y, 2);
  }
  expect_ok({"-i", tile_top.string(), "-o", tmp.File("frag0.jpx").string(),
             "-num_threads", "0", "-frag", "0,0,1,2", "Sdims={16,16}",
             "Stiles={8,8}"});

  // 3. JP2 features: palettes, TIFF metadata, -jp2_alpha, -jp2_aspect, boxes.
  expect_ok({"-i", bmp_pal.string(), "-o", tmp.File("pal_color.jp2").string(),
             "-num_threads", "0"});
  expect_ok({"-i", bmp_pal.string(), "-o", tmp.File("pal_none.jp2").string(),
             "-num_threads", "0", "-no_palette"});
  expect_ok({"-i", bmp_mono.string(), "-o", tmp.File("pal_mono.jp2").string(),
             "-num_threads", "0"});
  expect_ok({"-i", tif_rgba.string(), "-o", tmp.File("tif_meta.jp2").string(),
             "-num_threads", "0", "-rotate", "90"});
  // Include -jp2_alpha to avoid the nested `kdu_warning w;` self-deadlock at
  // kdu_compress.cpp:2757,2765 when premultiplied TIFF meets sLUM mismatch.
  expect_ok({"-i", tif_rgba.string(), "-o", tmp.File("tif_lum.jp2").string(),
             "-num_threads", "0", "-jp2_space", "sLUM", "-jp2_alpha"});

  const std::filesystem::path box1 = tmp.File("box1.bin");
  const std::filesystem::path box2 = tmp.File("box2.bin");
  WriteTextFile(box1, "xml \n<meta>kdu</meta>");
  WriteTextFile(box2, "");
  expect_ok({"-i", rgb.string() + "," + gray.string(), "-o",
             tmp.File("alpha_box.jp2").string(), "-num_threads", "0",
             "-jp2_alpha", "-jp2_aspect", "1.25", "-jp2_box",
             box1.string() + ",," + box2.string()});

  constexpr auto kJp2Spaces1 =
      std::to_array<std::string_view>({"sLUM", "iccLUM,2.4,0.055"});
  for (std::string_view sp : kJp2Spaces1) {
    expect_ok(
        {"-i", gray.string(), "-o",
         tmp.File("jp2_1_" + std::string(sp.substr(0, 4)) + ".jp2").string(),
         "-num_threads", "0", "-jp2_space", std::string(sp)});
  }
  constexpr auto kJp2Spaces3 = std::to_array<std::string_view>(
      {"sRGB", "sYCC", "iccRGB,2.4,0.055,0.64,0.33,0.30,0.60,0.15,0.06,D65",
       "iccRGB,2.2,0.0,0.64,0.33,0.30,0.60,0.15,0.06,D50"});
  int jp2_idx = 0;
  for (std::string_view sp : kJp2Spaces3) {
    expect_ok({"-i", rgb.string(), "-o",
               tmp.File("jp2_3_" + std::to_string(jp2_idx++) + ".jp2").string(),
               "-num_threads", "0", "-jp2_space", std::string(sp)});
  }

  // -jph_non_space (colour + non-colour on JPX; pure non-colour float on JPX).
  expect_ok({"-i", rgb.string(), "-o", tmp.File("nonsp.jpx").string(),
             "-num_threads", "0", "-jp2_space", "sLUM", "-jph_non_space",
             "524C,474C"});
  expect_ok({"-i", gray.string() + "," + gray.string(), "-o",
             tmp.File("nonsp_flt.jpx").string(), "-num_threads", "0",
             "-jph_non_space", "524C,474C", "-fprec", "16F5"});

  // 4. JPX colour spaces (-jpx_space) and multi-layer creation (-jpx_layers).
  constexpr auto kJpxSpaces1 =
      std::to_array<std::string_view>({"bilevel1", "bilevel2", "sLUM,10,2"});
  int jpx1_idx = 0;
  for (std::string_view sp : kJpxSpaces1) {
    expect_ok({"-i", gray.string(), "-o",
               tmp.File("jpx1_" + std::to_string(jpx1_idx++) + ".jpf").string(),
               "-num_threads", "0", "-jpx_space", std::string(sp)});
  }
  // YPbPr60_SPACE / YPbPr50_SPACE match kdu_compress.cpp:2682-2685 (diverging
  // from the -usage help text).
  constexpr auto kJpxSpaces3 = std::to_array<std::string_view>(
      {"YCbCr1", "YCbCr2", "YCbCr3", "PhotoYCC", "CMY", "CIELab", "CIEJab",
       "sRGB", "sYCC", "esRGB", "esYCC", "ROMMRGB", "YPbPr60_SPACE",
       "YPbPr50_SPACE"});
  int jpx3_idx = 0;
  for (std::string_view sp : kJpxSpaces3) {
    expect_ok({"-i", rgb.string(), "-o",
               tmp.File("jpx3_" + std::to_string(jpx3_idx++) + ".jpx").string(),
               "-num_threads", "0", "-jpx_space", std::string(sp)});
  }
  const std::string four_comp = rgb.string() + "," + gray.string();
  constexpr auto kJpxSpaces4 =
      std::to_array<std::string_view>({"CMYK", "YCCK"});
  for (std::string_view sp : kJpxSpaces4) {
    expect_ok({"-i", four_comp, "-o",
               tmp.File("jpx4_" + std::string(sp) + ".jpx").string(),
               "-num_threads", "0", "-jpx_space", std::string(sp)});
  }

  const std::filesystem::path multi_jpx = tmp.File("multi_star.jpx");
  {
    const RunResult mres = RunTool(
        bin, {"-i", rgb.string() + "," + rgb.string(), "-o", multi_jpx.string(),
              "-num_threads", "0", "-jpx_layers", "*"});
    ASSERT_EQ(mres.exit_code, 0) << mres.stderr_text;
    ExpectNonEmptyFile(multi_jpx);
  }
  expect_ok({"-i", tif_rgba.string() + "," + tif_rgba.string(), "-o",
             tmp.File("multi_premult.jpx").string(), "-num_threads", "0",
             "-jpx_layers", "2"});
  expect_ok({"-i", four_comp + "," + four_comp, "-o",
             tmp.File("multi_alpha_f.jpx").string(), "-num_threads", "0",
             "-jp2_alpha", "-jpx_layers", "2", "-fprec", "16F5"});
  // -jpx_space promotes even a .jp2 filename to JPX.
  expect_ok({"-i", gray.string(), "-o", tmp.File("promoted.jp2").string(),
             "-num_threads", "0", "-jpx_space", "sLUM"});
  {
    jp2_family_src fsrc;
    fsrc.open(multi_jpx.c_str());
    jpx_source jpx_in;
    ASSERT_GT(jpx_in.open(&fsrc, false), 0);
    int num_layers = 0;
    EXPECT_TRUE(jpx_in.count_compositing_layers(num_layers));
    EXPECT_EQ(num_layers, 2);
  }
}

}  // namespace
}  // namespace kdu_supp
