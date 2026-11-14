// CLI tests for third_party/kakadu/vendor/apps/kdu_expand/kdu_expand.cpp.

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
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
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_compressor.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_arch.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_threads.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

extern char** environ;

namespace kdu_supp {
namespace {

using kdu_core::kdu_byte;
using kdu_core::kdu_codestream;
using kdu_core::kdu_compressed_target;
using kdu_core::kdu_coords;
using kdu_core::kdu_int16;
using kdu_core::kdu_int32;
using kdu_core::kdu_thread_safe_message;
using kdu_core::siz_params;

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

class MemoryTarget final : public kdu_compressed_target {
 public:
  bool write(const kdu_byte* buf, int num_bytes) override {
    if (num_bytes > 0) data_.insert(data_.end(), buf, buf + num_bytes);
    return true;
  }
  const std::vector<kdu_byte>& data() const { return data_; }

 private:
  std::vector<kdu_byte> data_;
};

void WriteBytes(const std::filesystem::path& path,
                const std::vector<kdu_byte>& bytes) {
  std::ofstream ofs(path, std::ios::binary);
  ASSERT_TRUE(ofs.good());
  ofs.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(ofs.good());
}

void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
  std::ofstream ofs(path);
  ASSERT_TRUE(ofs.good());
  ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
  ASSERT_TRUE(ofs.good());
}

void CompressCustom(kdu_compressed_target* target, int width, int height,
                    int comps, int bit_depth, bool is_signed,
                    const std::vector<const char*>& siz_params_list,
                    const std::vector<const char*>& cod_params_list,
                    const char* comment = nullptr) {
  siz_params siz;
  siz.set(Scomponents, 0, 0, comps);
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Ssigned, 0, 0, is_signed);
  siz.set(Sprecision, 0, 0, bit_depth);
  for (const char* p : siz_params_list) {
    static_cast<kdu_core::kdu_params&>(siz).parse_string(p);
  }
  static_cast<kdu_core::kdu_params&>(siz).finalize();

  kdu_codestream cs;
  absl::Cleanup destroy_cs = [&cs] {
    if (cs.exists()) cs.destroy();
  };
  cs.create(&siz, target);
  cs.access_siz()->parse_string("Creversible=yes");
  for (const char* p : cod_params_list) {
    cs.access_siz()->parse_string(p);
  }
  cs.access_siz()->finalize_all();
  if (comment != nullptr) {
    cs.add_comment().put_text(comment);
  }

  std::vector<int> heights(comps);
  std::vector<int> widths(comps);
  std::vector<int> precs(comps, bit_depth);
  size_t total_samples = 0;
  for (int c = 0; c < comps; ++c) {
    kdu_core::kdu_dims d;
    cs.get_dims(c, d, false);
    widths[c] = d.size.x;
    heights[c] = d.size.y;
    total_samples += static_cast<size_t>(widths[c]) * heights[c];
  }

  kdu_stripe_compressor comp;
  comp.start(cs);
  if (bit_depth <= 8 && !is_signed) {
    std::vector<kdu_byte> pixels(total_samples);
    for (size_t i = 0; i < total_samples; ++i) {
      pixels[i] = static_cast<kdu_byte>((i * 23 + 7) & 0xFF);
    }
    std::vector<kdu_byte*> ptrs(comps);
    std::vector<int> row_gaps(comps);
    size_t off = 0;
    for (int c = 0; c < comps; ++c) {
      ptrs[c] = pixels.data() + off;
      row_gaps[c] = widths[c];
      off += static_cast<size_t>(widths[c]) * heights[c];
    }
    comp.push_stripe(ptrs.data(), heights.data(), nullptr, row_gaps.data(),
                     precs.data());
  } else {
    std::vector<kdu_int16> pixels(total_samples);
    const int mask = (1 << (bit_depth - 1)) - 1;
    for (size_t i = 0; i < total_samples; ++i) {
      const int v = static_cast<int>((i * 31 + 5) & mask);
      pixels[i] = static_cast<kdu_int16>(is_signed ? (v - mask / 2) : v);
    }
    std::vector<kdu_int16*> ptrs(comps);
    std::vector<int> row_gaps(comps);
    size_t off = 0;
    for (int c = 0; c < comps; ++c) {
      ptrs[c] = pixels.data() + off;
      row_gaps[c] = widths[c];
      off += static_cast<size_t>(widths[c]) * heights[c];
    }
    comp.push_stripe(ptrs.data(), heights.data(), nullptr, row_gaps.data(),
                     precs.data());
  }
  comp.finish();
}

void WriteRawJ2k(const std::filesystem::path& path, int w, int h, int comps,
                 int bit_depth, bool is_signed,
                 const std::vector<const char*>& siz_params_list,
                 const std::vector<const char*>& cod_params_list,
                 const char* comment = nullptr) {
  MemoryTarget tgt;
  CompressCustom(&tgt, w, h, comps, bit_depth, is_signed, siz_params_list,
                 cod_params_list, comment);
  WriteBytes(path, tgt.data());
}

void WriteJp2RgbaWithRes(const std::filesystem::path& path, int w, int h) {
  jp2_family_tgt tgt;
  tgt.open(path.c_str());
  jp2_target jp2_out;
  jp2_out.open(&tgt);

  siz_params siz;
  siz.set(Scomponents, 0, 0, 4);
  siz.set(Sdims, 0, 0, h);
  siz.set(Sdims, 0, 1, w);
  siz.set(Ssigned, 0, 0, false);
  siz.set(Sprecision, 0, 0, 8);
  static_cast<kdu_core::kdu_params&>(siz).finalize();

  jp2_out.access_dimensions().init(&siz);
  jp2_out.access_colour().init(JP2_sRGB_SPACE);
  jp2_channels ch = jp2_out.access_channels();
  ch.init(3);
  for (int c = 0; c < 3; ++c) ch.set_colour_mapping(c, c);
  ch.set_opacity_mapping(-1, 3);
  jp2_resolution res = jp2_out.access_resolution();
  res.init(1.25f);
  res.set_resolution(120.0f, true);
  res.set_resolution(150.0f, false);
  jp2_out.write_header();
  jp2_out.open_codestream(true);
  CompressCustom(&jp2_out, w, h, 4, 8, false, {}, {"Clevels=2"});
  jp2_out.close();
  tgt.close();
}

void WriteJp2PremultIccLumLowDisplayRes(const std::filesystem::path& path,
                                        int w, int h) {
  jp2_family_tgt tgt;
  tgt.open(path.c_str());
  jp2_target jp2_out;
  jp2_out.open(&tgt);

  siz_params siz;
  siz.set(Scomponents, 0, 0, 2);
  siz.set(Sdims, 0, 0, h);
  siz.set(Sdims, 0, 1, w);
  siz.set(Ssigned, 0, 0, false);
  siz.set(Sprecision, 0, 0, 8);
  static_cast<kdu_core::kdu_params&>(siz).finalize();

  jp2_out.access_dimensions().init(&siz);
  jp2_out.access_colour().init(2.4, 0.055);
  jp2_channels ch = jp2_out.access_channels();
  ch.init(1);
  ch.set_colour_mapping(0, 0);
  ch.set_premult_mapping(-1, 1);
  jp2_resolution res = jp2_out.access_resolution();
  res.init(1.0f);
  res.set_resolution(5.0f, true);
  jp2_out.write_header();
  jp2_out.open_codestream(true);
  CompressCustom(&jp2_out, w, h, 2, 8, false, {}, {"Clevels=1"});
  jp2_out.close();
  tgt.close();
}

void WriteJpxRich(const std::filesystem::path& path, int w, int h) {
  // Codestream 0: 1-comp 8-bit for palette16 & palette32 testing.
  MemoryTarget cs0_bytes;
  CompressCustom(&cs0_bytes, w, h, 1, 8, false, {}, {"Clevels=1"});
  // Codestream 1: 2-comp 16-bit signed SMAG for non-colour and FLOAT tests.
  MemoryTarget cs1_bytes;
  CompressCustom(&cs1_bytes, w, h, 2, 16, true, {},
                 {"Clevels=1", "NLType=SMAG"});

  jp2_family_tgt ftgt;
  ftgt.open(path.c_str());
  jpx_target tgt;
  tgt.open(&ftgt);

  // Codestream 0 with 3 8-bit LUTs (250 entries -> lut16 path).
  jpx_codestream_target cs0 = tgt.add_codestream();
  jp2_dimensions dims0 = cs0.access_dimensions();
  dims0.init(kdu_coords(h, w), 1, false);
  dims0.set_precision(0, 8, false);
  dims0.finalize_compatibility(0, 0, 0, 0, 0, 0);
  jp2_palette plt0 = cs0.access_palette();
  plt0.init(3, 250);
  std::vector<kdu_int32> lut8(250);
  for (int i = 0; i < 250; ++i) lut8[i] = i;
  for (int c = 0; c < 3; ++c) {
    ASSERT_TRUE(plt0.set_lut(c, lut8.data(), 8, false));
  }

  // Codestream 1 with 3 16-bit LUTs (256 entries -> lut32 path).
  jpx_codestream_target cs1 = tgt.add_codestream();
  jp2_dimensions dims1 = cs1.access_dimensions();
  dims1.init(kdu_coords(h, w), 2, false);
  dims1.set_precision(0, 16, true);
  dims1.set_precision(1, 16, true);
  dims1.finalize_compatibility(0, 0, 0, 0, 0, 0);
  jp2_palette plt1 = cs1.access_palette();
  plt1.init(3, 256);
  std::vector<kdu_int32> lut16(256);
  for (int i = 0; i < 256; ++i) lut16[i] = i * 256;
  for (int c = 0; c < 3; ++c) {
    ASSERT_TRUE(plt1.set_lut(c, lut16.data(), 16, false));
  }

  // Layer 0: cs0 via lut16 + JP2_vendor_SPACE.
  {
    jpx_layer_target lyr = tgt.add_layer();
    lyr.access_channels().init(3);
    for (int c = 0; c < 3; ++c) {
      ASSERT_TRUE(lyr.access_channels().set_colour_mapping(c, 0, c, 0));
    }
    auto uuid = std::to_array<kdu_byte>(
        {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    auto vdata = std::to_array<kdu_byte>({1, 0, 0, 0});
    lyr.add_colour().init(uuid.data(), 4, vdata.data());
  }
  // Layer 1: cs1 via lut32 + sRGB.
  {
    jpx_layer_target lyr = tgt.add_layer();
    lyr.access_channels().init(3);
    for (int c = 0; c < 3; ++c) {
      ASSERT_TRUE(lyr.access_channels().set_colour_mapping(c, 0, c, 1));
    }
    lyr.add_colour().init(JP2_sRGB_SPACE);
  }
  // Layer 2: non-colour channels on cs1 (no colour space needed for
  // non-colour).
  {
    jpx_layer_target lyr = tgt.add_layer();
    lyr.access_channels().init(0, 2);
    ASSERT_TRUE(
        lyr.access_channels().set_non_colour_mapping(0, 0x524C, 0, -1, 1));
    ASSERT_TRUE(
        lyr.access_channels().set_non_colour_mapping(1, 0x474C, 1, -1, 1));
  }
  // Layer 3: JP2_CHANNEL_FORMAT_FLOAT on cs1 (color + opacity).
  {
    jpx_layer_target lyr = tgt.add_layer();
    lyr.access_channels().init(1);
    const int exp_bits = 5;
    ASSERT_TRUE(lyr.access_channels().set_colour_mapping(
        0, 0, -1, 1, JP2_CHANNEL_FORMAT_FLOAT, &exp_bits));
    ASSERT_TRUE(lyr.access_channels().set_opacity_mapping(
        -1, 1, -1, 1, JP2_CHANNEL_FORMAT_FLOAT, &exp_bits));
    lyr.add_colour().init(JP2_sLUM_SPACE);
  }

  ASSERT_EQ(tgt.write_headers(), nullptr);
  {
    jp2_output_box* box = cs0.open_stream();
    ASSERT_NE(box, nullptr);
    ASSERT_TRUE(cs0.access_stream()->write(
        cs0_bytes.data().data(), static_cast<int>(cs0_bytes.data().size())));
    ASSERT_TRUE(box->close());
  }
  {
    jp2_output_box* box = cs1.open_stream();
    ASSERT_NE(box, nullptr);
    ASSERT_TRUE(cs1.access_stream()->write(
        cs1_bytes.data().data(), static_cast<int>(cs1_bytes.data().size())));
    ASSERT_TRUE(box->close());
  }
  ASSERT_EQ(tgt.write_metadata(), nullptr);
  ASSERT_TRUE(tgt.close());
  ftgt.close();
}

void ExpectNonEmptyFile(const std::filesystem::path& path) {
  std::error_code ec;
  ASSERT_TRUE(std::filesystem::exists(path, ec)) << path;
  EXPECT_GT(std::filesystem::file_size(path, ec), 0U) << path;
}

TEST(KduExpandCcTest, HelpVersionAndArgumentErrors) {
  const std::filesystem::path bin =
      FindBinary("third_party/kakadu/vendor/kdu_expand");
  ASSERT_TRUE(std::filesystem::exists(bin)) << bin;
  ScopedTempDir tmp("expand_args");

  const std::filesystem::path gray_j2k = tmp.File("gray.j2k");
  WriteRawJ2k(gray_j2k, 16, 16, 1, 8, false, {}, {"Clevels=2"});
  const std::filesystem::path rgb4_j2k = tmp.File("rgb4.j2k");
  WriteRawJ2k(rgb4_j2k, 16, 16, 4, 8, false, {}, {"Clevels=2"});
  const std::filesystem::path jp2 = tmp.File("rgba.jp2");
  WriteJp2RgbaWithRes(jp2, 16, 16);
  const std::filesystem::path out_pgm = tmp.File("out.pgm");

  EXPECT_EQ(RunTool(bin, {}).exit_code, 0);
  EXPECT_EQ(RunTool(bin, {"-u"}).exit_code, 0);
  const RunResult usage = RunTool(bin, {"-usage"});
  EXPECT_EQ(usage.exit_code, 0);
  EXPECT_THAT(usage.stdout_text, ::testing::HasSubstr("-record"));
  const RunResult ver = RunTool(bin, {"-v"});
  EXPECT_EQ(ver.exit_code, 0);
  EXPECT_THAT(ver.stdout_text, ::testing::HasSubstr("kdu_expand"));
  EXPECT_EQ(RunTool(bin, {"-version"}).exit_code, 0);

  const std::vector<std::vector<std::string>> bad_invocations = {
      {"-o", out_pgm.string(), "-num_threads", "0"},
      {"-i"},
      {"-i", gray_j2k.string(), "-o"},
      {"-i", gray_j2k.string(), "-jpx_layer"},
      {"-i", gray_j2k.string(), "-jpx_layer", "-1"},
      {"-i", gray_j2k.string(), "-rate"},
      {"-i", gray_j2k.string(), "-rate", "0"},
      {"-i", gray_j2k.string(), "-skip_components"},
      {"-i", gray_j2k.string(), "-skip_components", "-1"},
      {"-i", gray_j2k.string(), "-layers"},
      {"-i", gray_j2k.string(), "-layers", "0"},
      {"-i", gray_j2k.string(), "-reduce"},
      {"-i", gray_j2k.string(), "-reduce", "-1"},
      {"-i", gray_j2k.string(), "-rotate"},
      {"-i", gray_j2k.string(), "-rotate", "45"},
      {"-i", gray_j2k.string(), "-qa"},
      {"-i", gray_j2k.string(), "-qa", "bad"},
      {"-i", gray_j2k.string(), "-qa", "1.5"},
      {"-i", gray_j2k.string(), "-reduce", "1", "-qa", "0.1"},
      {"-i", gray_j2k.string(), "-num_threads"},
      {"-i", gray_j2k.string(), "-num_threads", "-1"},
      {"-i", gray_j2k.string(), "-num_threads", "2x"},
      {"-i", gray_j2k.string(), "-num_threads", "2,1X"},
      {"-i", gray_j2k.string(), "-double_buffering"},
      {"-i", gray_j2k.string(), "-double_buffering", "-1"},
      {"-i", gray_j2k.string(), "-progress"},
      {"-i", gray_j2k.string(), "-progress", "0"},
      {"-i", gray_j2k.string(), "-cpu"},
      {"-i", gray_j2k.string(), "-cpu", "-1"},
      {"-i", gray_j2k.string(), "-o", out_pgm.string(), "-no_decode"},
      {"-i", gray_j2k.string(), "-record"},
      {"-i", gray_j2k.string(), "-record", "/nonexistent_kdu_dir/rec.txt"},
      {"-i", gray_j2k.string(), "-num_threads", "0", "-fprec"},
      {"-i", gray_j2k.string(), "-num_threads", "0", "-fprec", "65"},
      {"-i", gray_j2k.string(), "-num_threads", "0", "-fprec", "8Z"},
      {"-i", gray_j2k.string(), "-num_threads", "0", "-region"},
      {"-i", gray_j2k.string(), "-num_threads", "0", "-region",
       "{0.0,0.0},{0.0,0.5}"},
      {"-i", gray_j2k.string(), "-num_threads", "0", "-bogus"},
      {"-i", jp2.string(), "-skip_components", "1", "-num_threads", "0"},
      {"-i", jp2.string(), "-codestream_components", "-num_threads", "0"},
      {"-i", jp2.string(), "-raw_components", "99", "-num_threads", "0"},
      {"-i", jp2.string(), "-jpx_layer", "99", "-num_threads", "0"},
      {"-i", rgb4_j2k.string(), "-o",
       tmp.File("mix.bmp").string() + "," + tmp.File("mix.pgm").string(),
       "-num_threads", "0"},
      {"-i", tmp.File("missing.j2k").string(), "-num_threads", "0"},
  };
  for (size_t i = 0; i < bad_invocations.size(); ++i) {
    const RunResult res = RunTool(bin, bad_invocations[i]);
    EXPECT_NE(res.exit_code, 0)
        << "Case " << i << ": " << JoinArgs(bad_invocations[i]);
  }
}

TEST(KduExpandCcTest, OutputFormatsFlagsAndSingleThread) {
  const std::filesystem::path bin =
      FindBinary("third_party/kakadu/vendor/kdu_expand");
  ASSERT_TRUE(std::filesystem::exists(bin)) << bin;
  ScopedTempDir tmp("expand_formats");

  const std::filesystem::path gray_j2k = tmp.File("gray_tiled.j2k");
  WriteRawJ2k(gray_j2k, 16, 16, 1, 8, false, {"Stiles={8,8}"},
              {"Clevels=2", "Clayers=2"}, "kdu_expand_comment");

  const std::filesystem::path rgb_j2k = tmp.File("rgb.j2k");
  WriteRawJ2k(rgb_j2k, 16, 16, 3, 8, false, {}, {"Clevels=2", "Clayers=2"});

  // Grayscale formats, diagnostics flags, switch file, record, and -no_decode.
  const std::filesystem::path switch_file = tmp.File("switches.txt");
  const std::filesystem::path sw_out = tmp.File("from_switch.pgm");
  const std::filesystem::path rec_file = tmp.File("recorded.txt");
  WriteTextFile(switch_file, "# comment line\n% another comment\n-quiet\n-o\n" +
                                 sw_out.string() + "\n");

  constexpr auto kGrayExts = std::to_array<std::string_view>(
      {".pgm", ".tif", ".raw", ".rawl", ".pgx"});
  for (std::string_view ext : kGrayExts) {
    const std::filesystem::path out = tmp.File("out_g" + std::string(ext));
    const RunResult res = RunTool(
        bin, {"-i", gray_j2k.string(), "-o", out.string(), "-num_threads", "0",
              "-double_buffering", "4", "-progress", "2", "-stats", "-mem",
              "-mem_limit", "-no_res", "-bc_noopt", "-com", "-cpu", "2"});
    EXPECT_EQ(res.exit_code, 0) << ext << ": " << res.stderr_text;
    ExpectNonEmptyFile(out);
  }
  EXPECT_EQ(RunTool(bin, {"-i", gray_j2k.string(), "-s", switch_file.string(),
                          "-num_threads", "0", "-record", rec_file.string(),
                          "-mem_limit", "4096", "-fussy", "-no_seek"})
                .exit_code,
            0);
  ExpectNonEmptyFile(sw_out);
  ExpectNonEmptyFile(rec_file);

  const RunResult no_dec = RunTool(
      bin, {"-i", gray_j2k.string(), "-no_decode", "-com", "-resilient_sop"});
  EXPECT_EQ(no_dec.exit_code, 0) << no_dec.stderr_text;
  EXPECT_THAT(no_dec.stdout_text, ::testing::HasSubstr("kdu_expand_comment"));

  const RunResult no_out = RunTool(
      bin, {"-i", gray_j2k.string(), "-num_threads", "0", "-resilient"});
  EXPECT_EQ(no_out.exit_code, 0) << no_out.stderr_text;

  // RGB formats, rotations, flip, region, layers, reduce, rate, precision, qa.
  constexpr auto kRgbExts =
      std::to_array<std::string_view>({".ppm", ".bmp", ".tif"});
  for (std::string_view ext : kRgbExts) {
    const std::filesystem::path out = tmp.File("out_rgb" + std::string(ext));
    const RunResult res = RunTool(
        bin, {"-i", rgb_j2k.string(), "-o", out.string(), "-num_threads", "0"});
    EXPECT_EQ(res.exit_code, 0) << ext << ": " << res.stderr_text;
    ExpectNonEmptyFile(out);
  }
  for (int angle : {0, 90, 180, 270, -90, 450}) {
    const std::filesystem::path out =
        tmp.File("rot_" + std::to_string(angle) + ".ppm");
    EXPECT_EQ(RunTool(bin, {"-i", rgb_j2k.string(), "-o", out.string(),
                            "-num_threads", "0", "-rotate",
                            std::to_string(angle), "-flip"})
                  .exit_code,
              0);
    ExpectNonEmptyFile(out);
  }
  const std::filesystem::path c0 = tmp.File("c0.pgm");
  const std::filesystem::path c1 = tmp.File("c1.pgm");
  const std::filesystem::path c2 = tmp.File("c2.pgm");
  EXPECT_EQ(RunTool(bin, {"-i", rgb_j2k.string(), "-o",
                          c0.string() + "," + c1.string() + "," + c2.string(),
                          "-num_threads", "0", "-codestream_components",
                          "-fprec", "8,8M,8L"})
                .exit_code,
            0);
  ExpectNonEmptyFile(c0);
  ExpectNonEmptyFile(c1);
  ExpectNonEmptyFile(c2);

  {
    const RunResult res = RunTool(
        bin, {"-i", rgb_j2k.string(), "-o", tmp.File("sub.ppm").string(),
              "-num_threads", "0", "-region", "{0.0,0.0},{0.5,0.5}", "-layers",
              "1", "-reduce", "1", "-rate", "40.0", "-precise"});
    EXPECT_EQ(res.exit_code, 0) << res.stderr_text;
  }
  {
    const RunResult res =
        RunTool(bin, {"-i", rgb_j2k.string(), "-o", tmp.File("vp.ppm").string(),
                      "-num_threads", "0", "-vprecise", "-qa", "0.25"});
    EXPECT_EQ(res.exit_code, 0) << res.stderr_text;
  }
  {
    const RunResult res =
        RunTool(bin, {"-i", rgb_j2k.string(), "-o",
                      tmp.File("fast.pgm").string(), "-num_threads", "0",
                      "-fastest", "-qa", "report", "-skip_components", "1"});
    EXPECT_EQ(res.exit_code, 0) << res.stderr_text;
  }

  if (HostFitsMaxThreads()) {
    EXPECT_EQ(RunTool(bin, {"-i", gray_j2k.string(), "-o",
                            tmp.File("default_threads.pgm").string()})
                  .exit_code,
              0);
  }
}

TEST(KduExpandCcTest, MultiThreadSubsamplingNltJp2AndJpxLayers) {
  const std::filesystem::path bin =
      FindBinary("third_party/kakadu/vendor/kdu_expand");
  ASSERT_TRUE(std::filesystem::exists(bin)) << bin;
  ScopedTempDir tmp("expand_containers");

  const std::filesystem::path rgb_tiled = tmp.File("rgb_tiled.j2k");
  WriteRawJ2k(rgb_tiled, 16, 16, 3, 8, false, {"Stiles={8,8}"}, {"Clevels=2"});

  const std::filesystem::path ycc420 = tmp.File("ycc420.j2k");
  WriteRawJ2k(ycc420, 16, 16, 3, 8, false,
              {"Sdims={16,16},{8,8},{8,8}", "Stiles={8,8}"},
              {"Clevels=1", "Cycc=no"});

  const std::filesystem::path nlt_j2k = tmp.File("nlt_smag.j2k");
  WriteRawJ2k(nlt_j2k, 16, 16, 1, 16, true, {}, {"Clevels=1", "NLType=SMAG"});

  const std::filesystem::path u16_j2k = tmp.File("linear16.j2k");
  WriteRawJ2k(u16_j2k, 16, 16, 1, 16, false, {}, {"Clevels=1"});

  const std::filesystem::path jp2_rgba = tmp.File("rgba.jp2");
  WriteJp2RgbaWithRes(jp2_rgba, 16, 16);

  const std::filesystem::path jp2_premult = tmp.File("premult.jp2");
  WriteJp2PremultIccLumLowDisplayRes(jp2_premult, 16, 16);

  const std::filesystem::path jpx_rich = tmp.File("rich.jpx");
  WriteJpxRich(jpx_rich, 16, 16);

  // 1. Multi-threaded variants: bound, unbound, overloaded T/C clamping,
  // progress.
  constexpr auto kThreadModes = std::to_array<std::string_view>(
      {"2", "U2", "2,1T,1C", "U3,1T,1C", "1,2T,2C"});
  int mt_idx = 0;
  for (std::string_view nt : kThreadModes) {
    const std::filesystem::path out =
        tmp.File("mt_" + std::to_string(mt_idx++) + ".ppm");
    const RunResult res =
        RunTool(bin, {"-i", rgb_tiled.string(), "-o", out.string(),
                      "-num_threads", std::string(nt), "-double_buffering", "4",
                      "-progress", "2", "-stats", "-mem", "-cpu", "1"});
    EXPECT_EQ(res.exit_code, 0) << "nt=" << nt << ": " << res.stderr_text;
    ExpectNonEmptyFile(out);
  }

  // 2. Vertically subsampled components (4:2:0) exercise component ratio
  // stepping.
  const std::string yuv_out = tmp.File("y.pgm").string() + "," +
                              tmp.File("u.pgm").string() + "," +
                              tmp.File("v.pgm").string();
  const auto expect_ok = [&](const std::vector<std::string>& args) {
    const RunResult res = RunTool(bin, args);
    EXPECT_EQ(res.exit_code, 0) << JoinArgs(args) << ": " << res.stderr_text;
  };
  expect_ok({"-i", ycc420.string(), "-o", yuv_out, "-num_threads", "2"});

  // 3. NLT SMAG codestream warnings and -fprec F5 behavior (single-component
  // input so kdu_expand.cpp:2627 idims.get_forced_precision(cmp, ...) indexing
  // aligns component idx and output channel idx).
  expect_ok({"-i", nlt_j2k.string(), "-o", tmp.File("nlt_nofprec.raw").string(),
             "-num_threads", "0"});
  expect_ok({"-i", nlt_j2k.string(), "-o", tmp.File("nlt_fprec.raw").string(),
             "-num_threads", "0", "-fprec", "16F5"});
  // Linear 16-bit codestream with -fprec F5 warning branch.
  expect_ok({"-i", u16_j2k.string(), "-o", tmp.File("lin_fprec.raw").string(),
             "-num_threads", "0", "-fprec", "16F5"});

  // 4. JP2 RGBA (opacity, resolution, transpose) + -no_alpha + -raw_components.
  expect_ok({"-i", jp2_rgba.string(), "-o", tmp.File("jp2_rgba.tif").string(),
             "-num_threads", "2", "-rotate", "90"});
  expect_ok({"-i", jp2_rgba.string(), "-o",
             tmp.File("jp2_noalpha.ppm").string(), "-num_threads", "0",
             "-no_alpha"});
  expect_ok({"-i", jp2_rgba.string(), "-o",
             tmp.File("jp2_rawcomp.pgm").string(), "-num_threads", "0",
             "-raw_components", "0", "-codestream_components",
             "-skip_components", "1"});

  // 5. JP2 premult + ICC luminance space + low display resolution (< 10 ppm).
  expect_ok({"-i", jp2_premult.string(), "-o",
             tmp.File("jp2_premult.tif").string(), "-num_threads", "0"});

  // 6. JPX layers 0..3: 16-bit/32-bit LUT palettes, vendor space, non-colour,
  // and JP2_CHANNEL_FORMAT_FLOAT on both colour & alpha.
  expect_ok({"-i", jpx_rich.string(), "-jpx_layer", "0", "-o",
             tmp.File("jpx_l0.ppm").string(), "-num_threads", "0"});
  expect_ok({"-i", jpx_rich.string(), "-jpx_layer", "1", "-o",
             tmp.File("jpx_l1.tif").string(), "-num_threads", "0"});
  expect_ok({"-i", jpx_rich.string(), "-jpx_layer", "2", "-o",
             tmp.File("jpx_l2a.raw").string() + "," +
                 tmp.File("jpx_l2b.raw").string(),
             "-num_threads", "0"});
  expect_ok({"-i", jpx_rich.string(), "-jpx_layer", "3", "-o",
             tmp.File("jpx_l3a.raw").string() + "," +
                 tmp.File("jpx_l3b.raw").string(),
             "-num_threads", "0"});
}

}  // namespace
}  // namespace kdu_supp
