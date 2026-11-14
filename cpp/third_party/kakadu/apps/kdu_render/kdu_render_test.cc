// CLI tests for third_party/kakadu/vendor/apps/kdu_render/kdu_render.cpp.

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
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jp2.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jpx.h"
#include "third_party/kakadu/vendor/apps/compressed_io/mj2.h"
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
using kdu_core::kdu_dims;
using kdu_core::kdu_thread_safe_message;
using kdu_core::siz_params;

// Vendor kdu_threads.cpp:2781,2869 accesses group->wait_managers[thrd_idx]
// out-of-bounds (reading adjacent thread_status == 4 as bool semaphore_valid,
// aborting UBSan under --config=asan-chlor) when add_thread() is called
// >= KDU_MAX_THREADS times. render_demo_5, render_demo_6, and bare -cpu
// unconditionally call kdu_get_num_processors() with no -num_threads cap.
bool HostFitsMaxThreads() {
  return kdu_core::kdu_get_num_processors() <= KDU_MAX_THREADS;
}

class GlobalErrorHandler final : public kdu_thread_safe_message {
 public:
  ~GlobalErrorHandler() noexcept override = default;
  void put_text(const char*) override {}
  void flush(bool end_of_message = false) override {
    kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) throw KDU_ERROR_EXCEPTION;
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

kdu_dims MakeDims(int x, int y, int w, int h) {
  kdu_dims d;
  d.pos = kdu_coords(x, y);
  d.size = kdu_coords(w, h);
  return d;
}

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
      std::string cmd;
      for (const std::string& a : args) {
        cmd += ' ';
        cmd += a;
      }
      ADD_FAILURE() << "Subprocess exceeded timeout/output budget for:" << cmd
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

void ConfigureAndCompressCustomToTarget(
    kdu_compressed_target* target, int width, int height, int comps,
    bool is_signed, const std::vector<const char*>& siz_params_list,
    const std::vector<const char*>& cod_params_list) {
  siz_params siz;
  siz.set(Scomponents, 0, 0, comps);
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Ssigned, 0, 0, is_signed);
  siz.set(Sprecision, 0, 0, 8);
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

  kdu_stripe_compressor comp;
  comp.start(cs);
  std::vector<int> heights(comps, height);
  std::vector<int> precs(comps, 8);
  if (is_signed) {
    std::vector<kdu_core::kdu_int16> spixels(static_cast<size_t>(width) *
                                             height * comps);
    for (size_t i = 0; i < spixels.size(); ++i) {
      spixels[i] = static_cast<kdu_core::kdu_int16>(
          static_cast<int>((i * 19 + 11) & 0xFF) - 128);
    }
    comp.push_stripe(spixels.data(), heights.data(), nullptr, nullptr, nullptr,
                     precs.data());
  } else {
    std::vector<kdu_byte> pixels(static_cast<size_t>(width) * height * comps);
    for (size_t i = 0; i < pixels.size(); ++i) {
      pixels[i] = static_cast<kdu_byte>((i * 19 + 11) & 0xFF);
    }
    comp.push_stripe(pixels.data(), heights.data(), nullptr, nullptr, nullptr,
                     precs.data());
  }
  comp.finish();
}

void ConfigureAndCompressToTarget(kdu_compressed_target* target, int width,
                                  int height, int comps, bool is_signed,
                                  const char* clevels_str) {
  ConfigureAndCompressCustomToTarget(target, width, height, comps, is_signed,
                                     {}, {clevels_str});
}

void WriteBytes(const std::filesystem::path& path,
                const std::vector<kdu_byte>& bytes) {
  std::ofstream ofs(path, std::ios::binary);
  ASSERT_TRUE(ofs.good());
  ofs.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(ofs.good());
}

void WriteRawJ2k(const std::filesystem::path& path, int w, int h, int comps,
                 bool is_signed, const char* clevels_str) {
  MemoryTarget tgt;
  ConfigureAndCompressToTarget(&tgt, w, h, comps, is_signed, clevels_str);
  WriteBytes(path, tgt.data());
}

void WriteJp2(const std::filesystem::path& path, int w, int h, int comps) {
  jp2_family_tgt tgt;
  tgt.open(path.c_str());
  jp2_target jp2_out;
  jp2_out.open(&tgt);

  siz_params siz;
  siz.set(Scomponents, 0, 0, comps);
  siz.set(Sdims, 0, 0, h);
  siz.set(Sdims, 0, 1, w);
  siz.set(Ssigned, 0, 0, false);
  siz.set(Sprecision, 0, 0, 8);
  static_cast<kdu_core::kdu_params&>(siz).finalize();

  jp2_out.access_dimensions().init(&siz);
  jp2_out.access_colour().init(comps == 1 ? JP2_sLUM_SPACE : JP2_sRGB_SPACE);
  jp2_out.write_header();
  jp2_out.open_codestream(true);
  ConfigureAndCompressToTarget(&jp2_out, w, h, comps, false, "Clevels=2");
  jp2_out.close();
  tgt.close();
}

void WriteJpxWithComposition(const std::filesystem::path& path, int w, int h) {
  MemoryTarget j2k;
  ConfigureAndCompressToTarget(&j2k, w, h, 3, false, "Clevels=2");

  jp2_family_tgt ftgt;
  ftgt.open(path.c_str());
  jpx_target tgt;
  tgt.open(&ftgt);

  jpx_codestream_target cs_tgt = tgt.add_codestream();
  jp2_dimensions dims = cs_tgt.access_dimensions();
  dims.init(kdu_coords(h, w), 3, false);
  for (int c = 0; c < 3; ++c) dims.set_precision(c, 8, false);
  dims.finalize_compatibility(0, 0, 0, 0, 0, 0);

  jpx_layer_target lyr = tgt.add_layer();
  lyr.access_channels().init(3);
  for (int c = 0; c < 3; ++c) {
    lyr.access_channels().set_colour_mapping(c, c, -1, 0);
  }
  lyr.add_colour().init(JP2_sRGB_SPACE);

  jpx_composition comp = tgt.access_composition();
  jx_frame* f0 = comp.add_frame(100, 0, true);
  ASSERT_NE(f0, nullptr);
  ASSERT_GE(comp.add_instruction(f0, 0, 0, MakeDims(0, 0, w, h),
                                 MakeDims(0, 0, w, h)),
            0);

  ASSERT_EQ(tgt.write_headers(), nullptr);
  jp2_output_box* stream_box = cs_tgt.open_stream();
  ASSERT_NE(stream_box, nullptr);
  ASSERT_TRUE(cs_tgt.access_stream()->write(
      j2k.data().data(), static_cast<int>(j2k.data().size())));
  ASSERT_TRUE(stream_box->close());
  ASSERT_EQ(tgt.write_metadata(), nullptr);
  ASSERT_TRUE(tgt.close());
  ftgt.close();
}

void WriteMj2SingleFrame(const std::filesystem::path& path, int w, int h) {
  jp2_family_tgt ftgt;
  ftgt.open(path.c_str());
  mj2_target tgt;
  tgt.open(&ftgt);
  auto* vt = tgt.add_video_track();
  ASSERT_NE(vt, nullptr);
  vt->access_colour().init(JP2_sRGB_SPACE);
  ASSERT_TRUE(vt->set_timescale(10));
  ASSERT_TRUE(vt->set_field_order(KDU_FIELDS_NONE));
  ASSERT_TRUE(vt->set_frame_period(1));
  vt->open_image();

  siz_params siz;
  siz.set(Scomponents, 0, 0, 3);
  siz.set(Sdims, 0, 0, h);
  siz.set(Sdims, 0, 1, w);
  siz.set(Ssigned, 0, 0, false);
  siz.set(Sprecision, 0, 0, 8);
  static_cast<kdu_core::kdu_params&>(siz).finalize();
  kdu_codestream cs;
  absl::Cleanup destroy_cs = [&cs] {
    if (cs.exists()) cs.destroy();
  };
  cs.create(&siz, vt);
  cs.access_siz()->parse_string("Creversible=yes");
  cs.access_siz()->parse_string("Clevels=1");
  cs.access_siz()->finalize_all();
  kdu_stripe_compressor comp;
  comp.start(cs);
  std::vector<kdu_byte> pixels(static_cast<size_t>(w) * h * 3, 77);
  std::array<int, 3> heights{h, h, h};
  std::array<int, 3> precs{8, 8, 8};
  comp.push_stripe(pixels.data(), heights.data(), nullptr, nullptr, nullptr,
                   precs.data());
  comp.finish();
  vt->close_image(cs);
  cs.destroy();
  tgt.close();
  ftgt.close();
}

void ExpectValidBmpHeader(const std::filesystem::path& bmp_path) {
  std::ifstream ifs(bmp_path, std::ios::binary);
  ASSERT_TRUE(ifs.good()) << bmp_path;
  std::array<char, 2> magic{};
  ifs.read(magic.data(), 2);
  EXPECT_EQ(magic[0], 'B');
  EXPECT_EQ(magic[1], 'M');
  std::error_code ec;
  const std::uintmax_t sz = std::filesystem::file_size(bmp_path, ec);
  EXPECT_FALSE(ec) << ec.message();
  EXPECT_GE(sz, 54U);
}

TEST(KduRenderCcTest, HelpVersionAndArgumentErrors) {
  const std::filesystem::path bin =
      FindBinary("third_party/kakadu/vendor/kdu_render");
  ASSERT_TRUE(std::filesystem::exists(bin)) << bin;
  ScopedTempDir tmp("render_args");
  const std::filesystem::path j2k = tmp.File("tiny.j2k");
  WriteRawJ2k(j2k, 8, 8, 3, false, "Clevels=2");
  const std::filesystem::path out = tmp.File("out.bmp");

  EXPECT_EQ(RunTool(bin, {}).exit_code, 0);
  EXPECT_EQ(RunTool(bin, {"-u"}).exit_code, 0);
  const RunResult usage = RunTool(bin, {"-usage"});
  EXPECT_EQ(usage.exit_code, 0);
  EXPECT_THAT(usage.stdout_text,
              ::testing::HasSubstr("The range of JPEG2000 family files"));
  const RunResult ver = RunTool(bin, {"-v"});
  EXPECT_EQ(ver.exit_code, 0);
  EXPECT_THAT(ver.stdout_text, ::testing::HasSubstr("kdu_render"));
  EXPECT_EQ(RunTool(bin, {"-version"}).exit_code, 0);

  // Argument parsing errors exit non-zero cleanly.
  const std::vector<std::vector<std::string>> bad_invocations = {
      {"-cpu", "-demo", "1", "-i", j2k.string()},
      {"-demo", "0", "-i", j2k.string(), "-o", out.string()},
      {"-demo", "7", "-i", j2k.string(), "-o", out.string()},
      {"-demo", "1", "-o", out.string()},
      {"-demo", "1", "-i"},
      {"-cpu", "-i", j2k.string(), "-o", out.string()},
      {"-demo", "1", "-i", j2k.string()},
      {"-demo", "1", "-i", j2k.string(), "-o"},
      {"-demo", "1", "-i", j2k.string(), "-o", out.string(), "-scale", "0"},
      {"-demo", "1", "-i", j2k.string(), "-o", out.string(), "-qlimit", "0"},
      {"-demo", "1", "-i", j2k.string(), "-o", out.string(), "-qlimit",
       "0.01@0"},
      {"-demo", "3", "-i", j2k.string(), "-o", out.string(), "-overlays",
       "bad"},
      {"-demo", "1", "-i", j2k.string(), "-o", out.string(), "-bogus"},
      {"-demo", "1", "-i", tmp.File("missing.j2k").string(), "-o",
       out.string()},
      {"-demo", "1", "-i", j2k.string(), "-o", "/nonexistent_kdu_dir/out.bmp"},
  };
  for (size_t i = 0; i < bad_invocations.size(); ++i) {
    SCOPED_TRACE(::testing::Message() << "bad_arg_case=" << i);
    EXPECT_NE(RunTool(bin, bad_invocations[i]).exit_code, 0);
  }
}

TEST(KduRenderCcTest, Demos1Through6AndCpuModeAcrossFormats) {
  const std::filesystem::path bin =
      FindBinary("third_party/kakadu/vendor/kdu_render");
  ASSERT_TRUE(std::filesystem::exists(bin)) << bin;
  ScopedTempDir tmp("render_demos");

  const std::filesystem::path raw_j2k = tmp.File("raw.j2k");
  const std::filesystem::path signed_j2k = tmp.File("signed.j2k");
  const std::filesystem::path jp2 = tmp.File("image.jp2");
  const std::filesystem::path jpx = tmp.File("comp.jpx");
  const std::filesystem::path mj2 = tmp.File("clip.mj2");

  WriteRawJ2k(raw_j2k, 16, 16, 3, false, "Clevels=3");
  WriteRawJ2k(signed_j2k, 8, 8, 1, true, "Clevels=2");
  WriteJp2(jp2, 16, 16, 3);
  WriteJpxWithComposition(jpx, 16, 16);
  // Track 1 holds the video frame (note kdu_render.cpp:1143 calls
  // mj2_in.get_next_track(0) cleanly on single-video-track streams).
  WriteMj2SingleFrame(mj2, 16, 16);

  // 1. CPU mode (-cpu 0 single-threaded, -cpu 2 multi-threaded, -cpu default).
  const RunResult cpu0 = RunTool(
      bin, {"-cpu", "0", "-i", raw_j2k.string(), "-scale", "0.25", "-qlimit",
            "0.01@96", "-true_zero", "-true_max", "-mem_limit"});
  EXPECT_EQ(cpu0.exit_code, 0) << cpu0.stderr_text;
  EXPECT_THAT(cpu0.stdout_text, ::testing::HasSubstr("single-threaded"));

  const RunResult cpu2 =
      RunTool(bin, {"-cpu", "2", "-i", jp2.string(), "-scale", "1.3", "-qlimit",
                    "0.02", "-mem_limit", "4096"});
  EXPECT_EQ(cpu2.exit_code, 0) << cpu2.stderr_text;
  EXPECT_THAT(cpu2.stdout_text, ::testing::HasSubstr("Granted memory"));

  if (HostFitsMaxThreads()) {
    const RunResult cpu_auto = RunTool(bin, {"-cpu", "-i", raw_j2k.string()});
    EXPECT_EQ(cpu_auto.exit_code, 0) << cpu_auto.stderr_text;
  }

  // 2. Demo 1 (raw only, fractional scale, tiny scale discard_levs++, qlimit).
  const std::filesystem::path out1a = tmp.File("out1a.bmp");
  EXPECT_EQ(RunTool(bin, {"-demo", "1", "-i", raw_j2k.string(), "-o",
                          out1a.string(), "-scale", "0.2", "-qlimit", "0.01@72",
                          "-mem_limit", "2048", "-true_zero", "-true_max"})
                .exit_code,
            0);
  ExpectValidBmpHeader(out1a);

  const std::filesystem::path out1b = tmp.File("out1b.bmp");
  EXPECT_EQ(RunTool(bin, {"-demo", "1", "-i", signed_j2k.string(), "-o",
                          out1b.string(), "-scale", "1.7"})
                .exit_code,
            0);
  ExpectValidBmpHeader(out1b);

  // 3. Demo 2 (raw and JP2).
  const std::filesystem::path out2a = tmp.File("out2a.bmp");
  EXPECT_EQ(RunTool(bin, {"-demo", "2", "-i", raw_j2k.string(), "-o",
                          out2a.string(), "-true_zero", "-mem_limit", "2048"})
                .exit_code,
            0);
  ExpectValidBmpHeader(out2a);

  const std::filesystem::path out2b = tmp.File("out2b.bmp");
  EXPECT_EQ(
      RunTool(bin, {"-demo", "2", "-i", jp2.string(), "-o", out2b.string(),
                    "-scale", "0.3", "-qlimit", "0.01@96", "-mem_limit"})
          .exit_code,
      0);
  ExpectValidBmpHeader(out2b);

  // 4. Demos 3..6 across raw, JP2, JPX (composition), and MJ2 + -overlays.
  const std::array<std::string, 4> inputs = {
      raw_j2k.string(),
      jp2.string(),
      jpx.string(),
      mj2.string(),
  };
  const int max_demo = HostFitsMaxThreads() ? 6 : 4;
  for (int demo = 3; demo <= max_demo; ++demo) {
    for (size_t k = 0; k < inputs.size(); ++k) {
      const std::filesystem::path out = tmp.File(
          "d" + std::to_string(demo) + "_" + std::to_string(k) + ".bmp");
      const RunResult res = RunTool(
          bin, {"-demo", std::to_string(demo), "-i", inputs[k], "-o",
                out.string(), "-scale", "1.3", "-overlays", "0.5", "-qlimit",
                "0.01@96", "-true_zero", "-true_max", "-mem_limit", "4096"});
      EXPECT_EQ(res.exit_code, 0) << "demo=" << demo << " input=" << inputs[k]
                                  << " err=" << res.stderr_text;
      ExpectValidBmpHeader(out);
    }
  }
}

TEST(KduRenderCcTest, EdgeScalesFlipsErrorsAndCatchPaths) {
  const std::filesystem::path bin =
      FindBinary("third_party/kakadu/vendor/kdu_render");
  ASSERT_TRUE(std::filesystem::exists(bin)) << bin;
  ScopedTempDir tmp("render_edge");

  // 1. Tiny scale (< min_safe_scale_prod) across demos 1, 3, 4 (and 5, 6 if
  // host core count fits within KDU_MAX_THREADS).
  const std::filesystem::path small_j2k = tmp.File("small.j2k");
  WriteRawJ2k(small_j2k, 16, 16, 3, false, "Clevels=2");
  for (int demo : {1, 3, 4, 5, 6}) {
    if (demo >= 5 && !HostFitsMaxThreads()) continue;
    const std::filesystem::path out =
        tmp.File("tiny_d" + std::to_string(demo) + ".bmp");
    const RunResult res =
        RunTool(bin, {"-demo", std::to_string(demo), "-i", small_j2k.string(),
                      "-o", out.string(), "-scale", "0.0001"});
    EXPECT_EQ(res.exit_code, 0) << "demo=" << demo << " " << res.stderr_text;
    ExpectValidBmpHeader(out);
  }

  // 2. Non-flippable Part-2 Cdecomp codestream across demos 1..6.
  const std::filesystem::path noflip_j2k = tmp.File("noflip.j2k");
  {
    MemoryTarget tgt;
    ConfigureAndCompressCustomToTarget(
        &tgt, 32, 32, 1, false, {"Sprofile=PART2", "Stiles={16,16}"},
        {"Clevels=2", "Cdecomp=B(H--:H--:H--),B(-:-:-)"});
    WriteBytes(noflip_j2k, tgt.data());
  }
  for (int demo : {1, 2}) {
    const std::filesystem::path out =
        tmp.File("noflip_d" + std::to_string(demo) + ".bmp");
    const RunResult res =
        RunTool(bin, {"-demo", std::to_string(demo), "-i", noflip_j2k.string(),
                      "-o", out.string()});
    // Demos 1 & 2 do not configure persistent codestreams, so querying
    // can_flip(true) and then calling change_appearance() throws into catch.
    EXPECT_NE(res.exit_code, 0) << "demo=" << demo;
  }
  for (int demo : {3, 4, 5, 6}) {
    if (demo >= 5 && !HostFitsMaxThreads()) continue;
    const std::filesystem::path out =
        tmp.File("noflip_d" + std::to_string(demo) + ".bmp");
    const RunResult res =
        RunTool(bin, {"-demo", std::to_string(demo), "-i", noflip_j2k.string(),
                      "-o", out.string()});
    EXPECT_EQ(res.exit_code, 0) << "demo=" << demo << " " << res.stderr_text;
    ExpectValidBmpHeader(out);
  }

  // 3. Heterogeneous tile DWT levels (T0 has 3, T1 has 1) with -scale 0.2.
  const std::filesystem::path hetero_j2k = tmp.File("hetero.j2k");
  {
    MemoryTarget tgt;
    ConfigureAndCompressCustomToTarget(&tgt, 32, 32, 1, false,
                                       {"Stiles={16,16}"},
                                       {"Clevels=3", "Clevels:T1=1"});
    WriteBytes(hetero_j2k, tgt.data());
  }
  EXPECT_EQ(RunTool(bin, {"-demo", "3", "-i", hetero_j2k.string(), "-o",
                          tmp.File("hetero3.bmp").string(), "-scale", "0.2"})
                .exit_code,
            0);
  EXPECT_EQ(RunTool(bin, {"-demo", "4", "-i", hetero_j2k.string(), "-o",
                          tmp.File("hetero4.bmp").string(), "-scale", "0.2"})
                .exit_code,
            0);
  EXPECT_NE(RunTool(bin, {"-demo", "1", "-i", hetero_j2k.string(), "-o",
                          tmp.File("hetero1.bmp").string(), "-scale", "0.2"})
                .exit_code,
            0);
  EXPECT_NE(RunTool(bin, {"-demo", "2", "-i", hetero_j2k.string(), "-o",
                          tmp.File("hetero2.bmp").string(), "-scale", "0.2"})
                .exit_code,
            0);
  EXPECT_NE(
      RunTool(bin, {"-cpu", "0", "-i", hetero_j2k.string(), "-scale", "0.2"})
          .exit_code,
      0);

  // 4. Wide 1-row codestream (> 65536 px) triggers cpu buffer growth branch.
  const std::filesystem::path wide_j2k = tmp.File("wide.j2k");
  WriteRawJ2k(wide_j2k, 65537, 1, 1, false, "Clevels=0");
  EXPECT_EQ(RunTool(bin, {"-cpu", "0", "-i", wide_j2k.string()}).exit_code, 0);

  // 5. JP2 signature-only file triggers empty-box error in demo 2 & cpu mode.
  const std::filesystem::path sig_jp2 = tmp.File("sig_only.jp2");
  WriteBytes(sig_jp2, {0x00, 0x00, 0x00, 0x0C, 'j', 'P', ' ', ' ', 0x0D, 0x0A,
                       0x87, 0x0A});
  EXPECT_NE(RunTool(bin, {"-demo", "2", "-i", sig_jp2.string(), "-o",
                          tmp.File("sig2.bmp").string()})
                .exit_code,
            0);
  EXPECT_NE(RunTool(bin, {"-cpu", "0", "-i", sig_jp2.string()}).exit_code, 0);

  // 6. Corrupt Tile 1 header inside a 2-tile codestream throws inside
  // process() for demos 2, 5, 6 and cpu 2, exercising exception cleanup paths.
  std::vector<kdu_byte> bad_tile1;
  {
    MemoryTarget tgt;
    ConfigureAndCompressCustomToTarget(&tgt, 16, 8, 1, false, {"Stiles={8,8}"},
                                       {"Clevels=1"});
    bad_tile1 = tgt.data();
    // Locate the second SOT marker (0xFF 0x90) and replace Tile 1's header
    // payload with a malformed QCD marker (Lqcd=1) so Tile 1 parsing throws.
    int sot_count = 0;
    size_t second_sot = bad_tile1.size();
    for (size_t i = 2; i + 12 <= bad_tile1.size(); ++i) {
      if (bad_tile1[i] == 0xFF && bad_tile1[i + 1] == 0x90) {
        if (++sot_count == 2) {
          second_sot = i;
          break;
        }
      }
    }
    ASSERT_LT(second_sot + 12, bad_tile1.size());
    bad_tile1[second_sot + 6] = 0x00;
    bad_tile1[second_sot + 7] = 0x00;
    bad_tile1[second_sot + 8] = 0x00;
    bad_tile1[second_sot + 9] = 20;
    bad_tile1.resize(second_sot + 12);
    // Append invalid QCD (0xFF 0x5C, length 1 < 2), SOD (0xFF 0x93), EOC.
    const std::vector<kdu_byte> corrupt_tail = {0xFF, 0x5C, 0x00, 0x01,
                                                0xFF, 0x93, 0xFF, 0xD9};
    bad_tile1.insert(bad_tile1.end(), corrupt_tail.begin(), corrupt_tail.end());
  }
  const std::filesystem::path bad_t1_path = tmp.File("bad_tile1.j2k");
  WriteBytes(bad_t1_path, bad_tile1);
  EXPECT_NE(RunTool(bin, {"-demo", "2", "-i", bad_t1_path.string(), "-o",
                          tmp.File("bt2.bmp").string()})
                .exit_code,
            0);
  if (HostFitsMaxThreads()) {
    EXPECT_NE(RunTool(bin, {"-demo", "5", "-i", bad_t1_path.string(), "-o",
                            tmp.File("bt5.bmp").string()})
                  .exit_code,
              0);
    EXPECT_NE(RunTool(bin, {"-demo", "6", "-i", bad_t1_path.string(), "-o",
                            tmp.File("bt6.bmp").string()})
                  .exit_code,
              0);
  }
  EXPECT_NE(RunTool(bin, {"-cpu", "2", "-i", bad_t1_path.string()}).exit_code,
            0);
}

}  // namespace
}  // namespace kdu_supp
