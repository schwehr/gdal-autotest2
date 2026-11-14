// CLI tests for third_party/kakadu/vendor/apps/kdu_jp2info/kdu_jp2info.cpp.

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
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_compressor.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

extern char** environ;

namespace kdu_supp {
namespace {

using kdu_core::kdu_byte;
using kdu_core::kdu_codestream;
using kdu_core::kdu_compressed_target;
using kdu_core::kdu_thread_safe_message;
using kdu_core::kdu_uint32;
using kdu_core::siz_params;

class GlobalErrorHandler final : public kdu_thread_safe_message {
 public:
  ~GlobalErrorHandler() noexcept override = default;
  void put_text(const char*) override {}
  void flush(bool end_of_message = false) override {
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

void WriteBytes(const std::filesystem::path& path,
                const std::vector<kdu_byte>& bytes) {
  std::ofstream ofs(path, std::ios::binary);
  ASSERT_TRUE(ofs.good()) << path;
  ofs.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(ofs.good());
}

void AppendU16BE(std::vector<kdu_byte>& out, uint16_t v) {
  out.push_back(static_cast<kdu_byte>(v >> 8));
  out.push_back(static_cast<kdu_byte>(v & 0xFF));
}

void AppendU32BE(std::vector<kdu_byte>& out, kdu_uint32 v) {
  out.push_back(static_cast<kdu_byte>((v >> 24) & 0xFF));
  out.push_back(static_cast<kdu_byte>((v >> 16) & 0xFF));
  out.push_back(static_cast<kdu_byte>((v >> 8) & 0xFF));
  out.push_back(static_cast<kdu_byte>(v & 0xFF));
}

// Encodes an 8x8 multi-tile RGB codestream via :kdu and optionally inserts COM
// marker segments (<256 bytes so vendor kdu_jp2info.cpp:355 high-byte Lmar/Rcom
// omission does not affect parsing) right before the first SOT (0xFF90).
std::vector<kdu_byte> MakeSyntheticCodestream(bool with_comments) {
  MemoryTarget tgt;
  siz_params siz;
  siz.set(Scomponents, 0, 0, 3);
  siz.set(Sdims, 0, 0, 8);
  siz.set(Sdims, 0, 1, 8);
  siz.set(Ssigned, 0, 0, false);
  siz.set(Sprecision, 0, 0, 8);
  siz.parse_string("Stiles={4,4}");
  static_cast<kdu_core::kdu_params&>(siz).finalize();

  kdu_codestream cs;
  absl::Cleanup destroy_cs = [&cs] {
    if (cs.exists()) cs.destroy();
  };
  cs.create(&siz, &tgt);
  cs.access_siz()->parse_string("Creversible=yes");
  cs.access_siz()->parse_string("Clevels=1");
  cs.access_siz()->finalize_all();

  kdu_stripe_compressor comp;
  comp.start(cs);
  std::vector<kdu_byte> pixels(8 * 8 * 3, 90);
  std::array<int, 3> heights{8, 8, 8};
  std::array<int, 3> precs{8, 8, 8};
  comp.push_stripe(pixels.data(), heights.data(), nullptr, nullptr, nullptr,
                   precs.data());
  comp.finish();

  std::vector<kdu_byte> data = tgt.data();
  if (!with_comments) return data;

  // Insert COM segments (binary Rcom=0, Latin Rcom=1, and custom Rcom=2) right
  // before the first SOT marker (0xFF90) in the main header.
  size_t sot_pos = 2;
  while (sot_pos + 1 < data.size()) {
    if (data[sot_pos] == 0xFF && data[sot_pos + 1] == 0x90) break;
    ++sot_pos;
  }
  EXPECT_LT(sot_pos + 1, data.size());

  std::vector<kdu_byte> coms;
  // Rcom = 0 (binary, 4 payload bytes)
  AppendU16BE(coms, 0xFF64);
  AppendU16BE(coms, 2 + 2 + 4);
  AppendU16BE(coms, 0);
  coms.insert(coms.end(), {0xDE, 0xAD, 0xBE, 0xEF});
  // Rcom = 1 (Latin text)
  constexpr std::string_view kText = "kakadu_info_comment";
  AppendU16BE(coms, 0xFF64);
  AppendU16BE(coms, static_cast<uint16_t>(2 + 2 + kText.size()));
  AppendU16BE(coms, 1);
  coms.insert(coms.end(), kText.begin(), kText.end());
  // Rcom = 2 (other registration)
  AppendU16BE(coms, 0xFF64);
  AppendU16BE(coms, 2 + 2 + 2);
  AppendU16BE(coms, 2);
  coms.insert(coms.end(), {0x11, 0x22});

  data.insert(data.begin() + sot_pos, coms.begin(), coms.end());
  return data;
}

constexpr kdu_uint32 FourCC(char a, char b, char c, char d) {
  return (static_cast<kdu_uint32>(static_cast<uint8_t>(a)) << 24) |
         (static_cast<kdu_uint32>(static_cast<uint8_t>(b)) << 16) |
         (static_cast<kdu_uint32>(static_cast<uint8_t>(c)) << 8) |
         static_cast<kdu_uint32>(static_cast<uint8_t>(d));
}

void WriteJp2WithExtraBoxes(const std::filesystem::path& path,
                            const std::vector<kdu_byte>& j2k,
                            bool append_rubber_unknown_box) {
  jp2_family_tgt tgt;
  tgt.open(path.c_str());
  jp2_target jp2_out;
  jp2_out.open(&tgt);

  siz_params siz;
  siz.set(Scomponents, 0, 0, 3);
  siz.set(Sdims, 0, 0, 8);
  siz.set(Sdims, 0, 1, 8);
  siz.set(Ssigned, 0, 0, false);
  siz.set(Sprecision, 0, 0, 8);
  static_cast<kdu_core::kdu_params&>(siz).finalize();

  jp2_out.access_dimensions().init(&siz);
  jp2_out.access_colour().init(JP2_sRGB_SPACE);
  jp2_out.write_header();
  jp2_out.open_codestream(false);
  EXPECT_TRUE(jp2_out.write(j2k.data(), static_cast<int>(j2k.size())));
  jp2_out.close();

  // Add an XML box (known textualizable non-superbox) and an unknown box.
  jp2_output_box xml_box;
  xml_box.open(&tgt, FourCC('x', 'm', 'l', ' '));
  constexpr std::string_view kXml = "<meta>hello_world_xml_payload</meta>";
  xml_box.write(reinterpret_cast<const kdu_byte*>(kXml.data()),
                static_cast<int>(kXml.size()));
  xml_box.close();

  jp2_output_box unkn_box;
  unkn_box.open(&tgt, FourCC('z', 'z', 'z', '1'));
  std::array<kdu_byte, 20> payload{};
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<kdu_byte>(0xA0 + i);
  }
  unkn_box.write(payload.data(), static_cast<int>(payload.size()));
  unkn_box.close();

  tgt.close();

  if (append_rubber_unknown_box) {
    // Append an explicit LBox=0 rubber unknown box at EOF (> -hex length so
    // jp2_input_box::read does not hit EOF and clear rubber_length).
    std::vector<kdu_byte> tail;
    AppendU32BE(tail, 0);
    AppendU32BE(tail, FourCC('r', 'u', 'b', 'b'));
    for (int i = 0; i < 40; ++i) {
      tail.push_back(static_cast<kdu_byte>(0x10 + i));
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::app);
    ASSERT_TRUE(ofs.good());
    ofs.write(reinterpret_cast<const char*>(tail.data()),
              static_cast<std::streamsize>(tail.size()));
  }
}

void WriteJpbFile(const std::filesystem::path& path,
                  const std::vector<kdu_byte>& j2k) {
  jp2_family_tgt tgt;
  tgt.open(path.c_str());
  jp2_output_box elsm;
  elsm.open(&tgt, jpb_elementary_stream_4cc);
  jp2_output_box codestream_box;
  codestream_box.open(&elsm, jp2_codestream_4cc);
  codestream_box.write(j2k.data(), static_cast<int>(j2k.size()));
  codestream_box.close();
  elsm.close();
  tgt.close();
}

TEST(KduJp2infoCcTest, HelpVersionAndArgumentErrors) {
  const std::filesystem::path bin =
      FindBinary("third_party/kakadu/vendor/kdu_jp2info");
  ASSERT_TRUE(std::filesystem::exists(bin)) << bin;
  ScopedTempDir tmp("jp2info_args");
  const std::filesystem::path dummy = tmp.File("dummy.j2k");
  WriteBytes(dummy, MakeSyntheticCodestream(false));

  // Usage, comprehensive usage, and version flags exit 0.
  EXPECT_EQ(RunTool(bin, {}).exit_code, 0);
  const RunResult u_res = RunTool(bin, {"-u"});
  EXPECT_EQ(u_res.exit_code, 0);
  EXPECT_THAT(u_res.stdout_text, ::testing::HasSubstr("-boxes"));

  const RunResult usage_res = RunTool(bin, {"-usage"});
  EXPECT_EQ(usage_res.exit_code, 0);
  EXPECT_THAT(usage_res.stdout_text, ::testing::HasSubstr("comprehensive"));

  const RunResult v_res = RunTool(bin, {"-v"});
  EXPECT_EQ(v_res.exit_code, 0);
  EXPECT_THAT(v_res.stdout_text, ::testing::HasSubstr("kdu_jp2info"));

  const RunResult ver_res = RunTool(bin, {"-version"});
  EXPECT_EQ(ver_res.exit_code, 0);

  // Argument validation errors exit non-zero cleanly without crashing.
  const std::vector<std::vector<std::string>> bad_invocations = {
      {"-siz"},
      {"-i", ""},
      {"-i", dummy.string(), "-boxes", "-1"},
      {"-i", dummy.string(), "-boxes", "bad"},
      {"-i", dummy.string(), "-hex", "0"},
      {"-i", dummy.string(), "-hex", "bad"},
      {"-i", dummy.string(), "-bogus_flag"},
      {"-i", tmp.File("missing.j2k").string()},
  };
  for (size_t i = 0; i < bad_invocations.size(); ++i) {
    SCOPED_TRACE(::testing::Message() << "bad_arg_case=" << i);
    EXPECT_NE(RunTool(bin, bad_invocations[i]).exit_code, 0);
  }
}

TEST(KduJp2infoCcTest, RawCodestreamJp2AndJpbInspection) {
  const std::filesystem::path bin =
      FindBinary("third_party/kakadu/vendor/kdu_jp2info");
  ASSERT_TRUE(std::filesystem::exists(bin)) << bin;
  ScopedTempDir tmp("jp2info_valid");

  const std::vector<kdu_byte> j2k = MakeSyntheticCodestream(true);
  const std::filesystem::path j2k_path = tmp.File("sample.j2k");
  WriteBytes(j2k_path, j2k);

  // 1. Raw codestream: default vs -siz -com.
  const RunResult raw_basic = RunTool(bin, {"-i", j2k_path.string()});
  EXPECT_EQ(raw_basic.exit_code, 0);
  EXPECT_THAT(raw_basic.stdout_text,
              ::testing::HasSubstr("<tiles> 4 </tiles>"));

  const RunResult raw_full =
      RunTool(bin, {"-i", j2k_path.string(), "-siz", "-com"});
  EXPECT_EQ(raw_full.exit_code, 0);
  EXPECT_THAT(raw_full.stdout_text, ::testing::HasSubstr("<SIZ>"));
  EXPECT_THAT(raw_full.stdout_text, ::testing::HasSubstr("type=\"Latin\""));
  EXPECT_THAT(raw_full.stdout_text,
              ::testing::HasSubstr("kakadu_info_comment"));
  EXPECT_THAT(raw_full.stdout_text, ::testing::HasSubstr("type=\"binary\""));

  // 2. JP2 family file: skeleton only, limited -boxes (exercises unexpanded
  // bytes), and full -boxes + -hex + rubber unknown box.
  const std::filesystem::path jp2_path = tmp.File("sample.jp2");
  WriteJp2WithExtraBoxes(jp2_path, j2k, /*append_rubber_unknown_box=*/true);

  const RunResult jp2_skel = RunTool(bin, {"-i", jp2_path.string()});
  EXPECT_EQ(jp2_skel.exit_code, 0);
  EXPECT_THAT(jp2_skel.stdout_text, ::testing::HasSubstr("<JP2_family_file>"));
  EXPECT_THAT(jp2_skel.stdout_text, ::testing::HasSubstr("<unknown_box"));

  const RunResult jp2_trunc_box =
      RunTool(bin, {"-i", jp2_path.string(), "-boxes", "4"});
  EXPECT_EQ(jp2_trunc_box.exit_code, 0);
  EXPECT_THAT(jp2_trunc_box.stdout_text,
              ::testing::HasSubstr("<unexpanded bytes="));

  const RunResult jp2_full = RunTool(
      bin, {"-i", jp2_path.string(), "-boxes", "256", "-hex", "16", "-siz"});
  EXPECT_EQ(jp2_full.exit_code, 0);
  EXPECT_THAT(jp2_full.stdout_text, ::testing::HasSubstr("<hexdump>"));
  EXPECT_THAT(jp2_full.stdout_text, ::testing::HasSubstr("body=\"rubber\""));
  EXPECT_THAT(jp2_full.stdout_text, ::testing::HasSubstr("<SIZ>"));

  // 3. Elementary broadcast stream (.jpb) starting with 'elsm'.
  const std::filesystem::path jpb_path = tmp.File("stream.jpb");
  WriteJpbFile(jpb_path, j2k);
  const RunResult jpb_res =
      RunTool(bin, {"-i", jpb_path.string(), "-boxes", "64", "-siz"});
  EXPECT_EQ(jpb_res.exit_code, 0);
  EXPECT_THAT(jpb_res.stdout_text,
              ::testing::HasSubstr("<elementary_broadcast_stream>"));
}

TEST(KduJp2infoCcTest, MalformedInputsProduceCleanErrors) {
  const std::filesystem::path bin =
      FindBinary("third_party/kakadu/vendor/kdu_jp2info");
  ASSERT_TRUE(std::filesystem::exists(bin)) << bin;
  ScopedTempDir tmp("jp2info_err");

  std::vector<kdu_byte> j2k_bad_com = MakeSyntheticCodestream(false);
  size_t sot = 2;
  while (sot + 1 < j2k_bad_com.size() &&
         !(j2k_bad_com[sot] == 0xFF && j2k_bad_com[sot + 1] == 0x90)) {
    ++sot;
  }
  j2k_bad_com.resize(sot);
  AppendU16BE(j2k_bad_com, 0xFF64);
  AppendU16BE(j2k_bad_com, 20);
  j2k_bad_com.insert(j2k_bad_com.end(), {0x00, 0x01, 'A'});

  const std::filesystem::path bad_jp2c = tmp.File("bad_jp2c.jp2");
  WriteJp2WithExtraBoxes(bad_jp2c, {0x00, 0x00, 0x00, 0x00}, false);

  struct MalformedCase {
    std::string filename;
    std::vector<kdu_byte> bytes;
    std::vector<std::string> extra_args;
  };
  const std::vector<MalformedCase> cases = {
      {"tiny.bin", {0x00}, {}},
      {"bad.bin", {'N', 'O', 'T', 'J', 'P', '2'}, {}},
      {"no_ftyp.jp2",
       {0x00, 0x00, 0x00, 0x0C, 'j',  'P',  ' ', ' ', 0x0D, 0x0A,
        0x87, 0x0A, 0x00, 0x00, 0x00, 0x08, 'f', 'r', 'e',  'e'},
       {}},
      {"no_siz.j2k", {0xFF, 0x4F, 0xFF, 0xD9}, {}},
      {"zero_siz.j2k", {0xFF, 0x4F, 0xFF, 0x51, 0x00, 0x02}, {}},
      {"trunc_siz.j2k", {0xFF, 0x4F, 0xFF, 0x51, 0x00, 0x29, 0x00}, {}},
      {"trunc_com.j2k", j2k_bad_com, {"-com"}},
  };
  for (const MalformedCase& c : cases) {
    SCOPED_TRACE(c.filename);
    const std::filesystem::path p = tmp.File(c.filename);
    WriteBytes(p, c.bytes);
    std::vector<std::string> args = {"-i", p.string()};
    args.insert(args.end(), c.extra_args.begin(), c.extra_args.end());
    EXPECT_NE(RunTool(bin, args).exit_code, 0);
  }
  EXPECT_NE(RunTool(bin, {"-i", bad_jp2c.string()}).exit_code, 0);
}

}  // namespace
}  // namespace kdu_supp
