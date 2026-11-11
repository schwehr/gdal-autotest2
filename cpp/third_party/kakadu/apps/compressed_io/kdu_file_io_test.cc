// Unit tests for inline helpers and wrappers in
// third_party/kakadu/vendor/apps/compressed_io/kdu_file_io.h.

#include "third_party/kakadu/vendor/apps/compressed_io/kdu_file_io.h"

#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>  // NOLINT
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <system_error>  // NOLINT(build/c++11)

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_elementary.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_supp {
namespace {

class ThrowingErrorHandler final : public kdu_core::kdu_thread_safe_message {
 public:
  void put_text(const char* /*text*/) override {}
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      kdu_core::kdu_thread_safe_message::flush(true);
      throw KDU_ERROR_EXCEPTION;
    }
    kdu_core::kdu_thread_safe_message::flush(false);
  }
};

class SilentWarningHandler final : public kdu_core::kdu_thread_safe_message {};

kdu_core::kdu_message* GlobalErrorHandler() {
  static auto* const handler = new ThrowingErrorHandler();
  return handler;
}

kdu_core::kdu_message* GlobalWarningHandler() {
  static auto* const handler = new SilentWarningHandler();
  return handler;
}

class KduFileIoTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    kdu_core::kdu_customize_errors(GlobalErrorHandler());
    kdu_core::kdu_customize_warnings(GlobalWarningHandler());
  }
};

const ::testing::Environment* const kKduFileIoEnv =
    ::testing::AddGlobalTestEnvironment(new KduFileIoTestEnvironment());

std::string ReadWholeFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

template <typename SourceT>
void VerifyFileSource(const std::filesystem::path& path,
                      const std::filesystem::path& missing_path) {
  SourceT empty_src;
  EXPECT_FALSE(empty_src.exists());
  EXPECT_TRUE(!empty_src);
  EXPECT_EQ(-1, empty_src.get_pos());
  EXPECT_EQ(0, empty_src.get_capabilities());
  EXPECT_EQ(nullptr, empty_src.get_membroker());
  EXPECT_TRUE(empty_src.close());

  EXPECT_THROW((void)SourceT(missing_path.c_str()), kdu_core::kdu_exception);

  SourceT opened(path.c_str(), /*allow_seeks=*/true);
  EXPECT_TRUE(opened.exists());
  EXPECT_FALSE(!opened);
  EXPECT_EQ(KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE,
            opened.get_capabilities());
  EXPECT_EQ(0, opened.get_pos());

  std::array<kdu_core::kdu_byte, 8> buf = {};
  EXPECT_EQ(4, opened.read(buf.data(), 4));
  EXPECT_EQ(4, opened.get_pos());
  EXPECT_EQ('A', buf[0]);
  EXPECT_EQ('D', buf[3]);

  EXPECT_TRUE(opened.seek(2));
  EXPECT_EQ(2, opened.get_pos());
  EXPECT_EQ(3, opened.read(buf.data(), 3));
  EXPECT_EQ('C', buf[0]);
  EXPECT_EQ('E', buf[2]);
  EXPECT_TRUE(opened.close());
  EXPECT_FALSE(opened.exists());

  kdu_core::kdu_membroker broker;
  ASSERT_TRUE(opened.open(path.c_str(), /*allow_seeks=*/false,
                          /*return_on_failure=*/false, &broker));
  EXPECT_EQ(&broker, opened.get_membroker());
  EXPECT_EQ(KDU_SOURCE_CAP_SEQUENTIAL, opened.get_capabilities());
  EXPECT_FALSE(opened.seek(0));
  EXPECT_EQ(4, opened.read(buf.data(), 4));
  EXPECT_TRUE(opened.close());
  EXPECT_EQ(nullptr, opened.get_membroker());
}

template <typename TargetT>
void VerifyFileTarget(const std::filesystem::path& path) {
  // Default-constructed targets leave restore_pos and cur_pos uninitialized,
  // so only check state/close/start_rewrite prior to calling open().
  TargetT default_tgt;
  EXPECT_FALSE(default_tgt.exists());
  EXPECT_TRUE(!default_tgt);
  EXPECT_FALSE(default_tgt.start_rewrite(0));
  EXPECT_TRUE(default_tgt.close());

  constexpr std::array<kdu_core::kdu_byte, 8> kInitial = {'0', '1', '2', '3',
                                                          '4', '5', '6', '7'};
  constexpr std::array<kdu_core::kdu_byte, 2> kPatchAb = {'A', 'B'};
  constexpr std::array<kdu_core::kdu_byte, 4> kPatchWxyz = {'W', 'X', 'Y', 'Z'};
  constexpr std::array<kdu_core::kdu_byte, 1> kNine = {'9'};

  {
    TargetT tgt(path.c_str(), /*append_to_existing=*/false);
    EXPECT_TRUE(tgt.exists());
    EXPECT_FALSE(!tgt);
    EXPECT_FALSE(tgt.end_rewrite());
    std::array<kdu_core::kdu_byte, 4> tail = {};
    EXPECT_FALSE(tgt.strip_tail(tail.data(), 2));

    EXPECT_TRUE(tgt.write(kInitial.data(), static_cast<int>(kInitial.size())));
    EXPECT_FALSE(tgt.start_rewrite(-1));
    EXPECT_FALSE(tgt.start_rewrite(100));
    EXPECT_TRUE(tgt.start_rewrite(4));
    EXPECT_FALSE(tgt.start_rewrite(1));  // Already in a rewrite.
    EXPECT_TRUE(tgt.write(kPatchAb.data(), static_cast<int>(kPatchAb.size())));
    EXPECT_TRUE(tgt.end_rewrite());  // advance == 2 != 0.

    EXPECT_TRUE(tgt.start_rewrite(2));
    // Clamp write inside rewrite to restore_pos - cur_pos == 2 bytes ('W','X').
    EXPECT_FALSE(
        tgt.write(kPatchWxyz.data(), static_cast<int>(kPatchWxyz.size())));
    EXPECT_TRUE(tgt.end_rewrite());  // advance == 0.
    EXPECT_TRUE(tgt.close());
  }
  EXPECT_EQ("0123ABWX", ReadWholeFile(path));

  kdu_core::kdu_membroker broker;
  {
    TargetT append_tgt;
    ASSERT_TRUE(append_tgt.open(path.c_str(), /*append_to_existing=*/true,
                                /*return_on_failure=*/false, &broker));
    EXPECT_EQ(&broker, append_tgt.get_membroker());
    std::array<kdu_core::kdu_byte, 4> tail = {};
    EXPECT_FALSE(append_tgt.strip_tail(tail.data(), 100));
    ASSERT_TRUE(append_tgt.strip_tail(tail.data(), 3));
    EXPECT_EQ('B', tail[0]);
    EXPECT_EQ('W', tail[1]);
    EXPECT_EQ('X', tail[2]);
    // can_strip_tail becomes false after a successful strip_tail().
    EXPECT_FALSE(append_tgt.strip_tail(tail.data(), 1));
    EXPECT_TRUE(append_tgt.write(kNine.data(), 1));
    EXPECT_TRUE(append_tgt.close());
    EXPECT_EQ(nullptr, append_tgt.get_membroker());
  }
  EXPECT_EQ("0123A9WX", ReadWholeFile(path));
}

TEST(KduFileIoTest, FtellAndFseekWorkAcrossOrigins) {
  const std::filesystem::path file_path =
      std::filesystem::path(::testing::TempDir()) / "ftell_fseek.bin";
  absl::Cleanup remove_file = [&file_path] {
    std::error_code ec;
    std::filesystem::remove(file_path, ec);
  };

  FILE* fp = std::fopen(file_path.c_str(), "w+b");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup close_fp = [&fp] {
    if (fp != nullptr) std::fclose(fp);
  };

  constexpr std::array<char, 6> kPayload = {'a', 'b', 'c', 'd', 'e', 'f'};
  ASSERT_EQ(kPayload.size(),
            std::fwrite(kPayload.data(), 1, kPayload.size(), fp));
  EXPECT_EQ(6, kdu_ftell(fp));
  EXPECT_EQ(0, kdu_fseek(fp, 2));
  EXPECT_EQ(2, kdu_ftell(fp));
  EXPECT_EQ(0, kdu_fseek(fp, 2, SEEK_CUR));
  EXPECT_EQ(4, kdu_ftell(fp));
  EXPECT_EQ(0, kdu_fseek(fp, 0, SEEK_END));
  EXPECT_EQ(6, kdu_ftell(fp));
}

TEST(KduFileIoTest, PlatformFileOpenAppendMapReadWriteAndPipe) {
  const std::filesystem::path file_path =
      std::filesystem::path(::testing::TempDir()) / "platform_file.bin";
  const std::filesystem::path mapped_fp_path =
      std::filesystem::path(::testing::TempDir()) / "platform_map_fp.bin";
  const std::filesystem::path missing_path =
      std::filesystem::path(::testing::TempDir()) / "missing_pf.bin";
  absl::Cleanup remove_files = [&] {
    std::error_code ec;
    std::filesystem::remove(file_path, ec);
    std::filesystem::remove(mapped_fp_path, ec);
  };

  kdu_platform_file pf;
  std::array<kdu_core::kdu_byte, 16> io_buf = {};
  EXPECT_FALSE(pf.exists());
  EXPECT_TRUE(!pf);
  EXPECT_FALSE(pf.check_seekable());
  EXPECT_FALSE(pf.set_non_blocking());
  EXPECT_EQ(0, pf.get_pos());
  EXPECT_EQ(0, pf.seek(4));
  EXPECT_EQ(0, pf.read(io_buf.data(), 4));
  EXPECT_EQ(0, pf.write(io_buf.data(), 4));
  EXPECT_EQ(0, pf.read_once(io_buf.data(), 4, false));
  EXPECT_EQ(0, pf.write_once(io_buf.data(), 4, false));
  EXPECT_FALSE(pf.map(nullptr, false));
  EXPECT_FALSE(pf.open(missing_path.c_str(), /*for_writing=*/false));
  EXPECT_FALSE(pf.append(missing_path.c_str()));

  kdu_platform_file alias;
  EXPECT_FALSE(alias.map(pf));  // Mapping a closed file fails cleanly.

  ASSERT_TRUE(pf.open(file_path.c_str(), /*for_writing=*/true));
  EXPECT_TRUE(pf.exists());
  EXPECT_FALSE(!pf);
  EXPECT_EQ(0, pf.write(io_buf.data(), 0));
  EXPECT_EQ(0, pf.write_once(io_buf.data(), 0, false));
  std::array<kdu_core::kdu_byte, 4> part1 = {'A', 'B', 'C', 'D'};
  std::array<kdu_core::kdu_byte, 4> part2 = {'E', 'F', 'G', 'H'};
  EXPECT_EQ(4, pf.write(part1.data(), 4));
  EXPECT_EQ(4, pf.write_once(part2.data(), 4, /*blocking=*/true));
  pf.close();

  ASSERT_TRUE(pf.append(file_path.c_str()));
  EXPECT_EQ(8, pf.get_pos());
  std::array<kdu_core::kdu_byte, 2> part3 = {'I', 'J'};
  EXPECT_EQ(2, pf.write_once(part3.data(), 2, /*blocking=*/false));
  EXPECT_EQ(10, pf.seek(-1));  // SEEK_END.
  pf.close();

  ASSERT_TRUE(pf.open(file_path.c_str(), /*for_writing=*/false));
  EXPECT_TRUE(pf.check_seekable());
  // Writes on a read-only descriptor return 0 cleanly.
  EXPECT_EQ(0, pf.write(part1.data(), 4));
  EXPECT_EQ(0, pf.write_once(part1.data(), 4, false));
  EXPECT_EQ(2, pf.seek(2));
  EXPECT_EQ(2, pf.get_pos());
  EXPECT_EQ(0, pf.read(io_buf.data(), 0));
  EXPECT_EQ(0, pf.read_once(io_buf.data(), 0, false));
  EXPECT_EQ(4, pf.read(io_buf.data(), 4));
  EXPECT_EQ('C', io_buf[0]);
  EXPECT_EQ('F', io_buf[3]);
  EXPECT_EQ(4, pf.read_once(io_buf.data(), 8, /*blocking=*/true));
  EXPECT_EQ('G', io_buf[0]);
  EXPECT_EQ('J', io_buf[3]);
  // Reads at EOF return 0 cleanly.
  EXPECT_EQ(0, pf.read(io_buf.data(), 4));
  EXPECT_EQ(0, pf.read_once(io_buf.data(), 4, false));

  // Mapping shares fd without transferring ownership on alias.close().
  ASSERT_TRUE(alias.map(pf));
  EXPECT_EQ(4, alias.seek(4));
  EXPECT_EQ(2, alias.read(io_buf.data(), 2));
  EXPECT_EQ('E', io_buf[0]);
  alias.close();
  EXPECT_TRUE(pf.exists());
  pf.close();

  // Map a writable FILE* positioned past offset 0 (triggers fflush +
  // seek(pos)).
  {
    FILE* write_fp = std::fopen(mapped_fp_path.c_str(), "w+b");
    ASSERT_NE(nullptr, write_fp);
    absl::Cleanup close_write_fp = [&write_fp] { std::fclose(write_fp); };
    ASSERT_EQ(4u, std::fwrite("abcd", 1, 4, write_fp));
    kdu_platform_file mapped_pf;
    ASSERT_TRUE(mapped_pf.map(write_fp, /*for_writing=*/true));
    EXPECT_EQ(4, mapped_pf.get_pos());
    EXPECT_EQ(2, mapped_pf.write(part1.data(), 2));
    mapped_pf.close();
  }
  EXPECT_EQ("abcdAB", ReadWholeFile(mapped_fp_path));

  // Non-blocking POSIX pipe behavior.
  std::array<int, 2> pipe_fds = {-1, -1};
  ASSERT_EQ(0, ::pipe(pipe_fds.data()));
  FILE* pipe_fp = nullptr;
  absl::Cleanup close_pipe = [&pipe_fds, &pipe_fp] {
    if (pipe_fp != nullptr) std::fclose(pipe_fp);
    if (pipe_fds[0] >= 0) ::close(pipe_fds[0]);
    if (pipe_fds[1] >= 0) ::close(pipe_fds[1]);
  };
  pipe_fp = ::fdopen(pipe_fds[0], "rb");
  ASSERT_NE(nullptr, pipe_fp);
  pipe_fds[0] = -1;

  kdu_platform_file pipe_pf;
  ASSERT_TRUE(pipe_pf.map(pipe_fp, /*for_writing=*/false));
  EXPECT_FALSE(pipe_pf.check_seekable());
  EXPECT_TRUE(pipe_pf.set_non_blocking());
  EXPECT_TRUE(pipe_pf.set_non_blocking());
  // Non-blocking read on an empty pipe returns -1 (EAGAIN); never pass
  // blocking=true on an empty pipe because select(1, ...) watches only fd 0.
  EXPECT_EQ(-1, pipe_pf.read_once(io_buf.data(), 4, /*blocking=*/false));
  constexpr std::array<char, 3> kPipeBytes = {'x', 'y', 'z'};
  ASSERT_EQ(3, ::write(pipe_fds[1], kPipeBytes.data(), kPipeBytes.size()));
  EXPECT_EQ(3, pipe_pf.read_once(io_buf.data(), 4, /*blocking=*/false));
  EXPECT_EQ('x', io_buf[0]);
  EXPECT_EQ('z', io_buf[2]);
  ::close(pipe_fds[1]);
  pipe_fds[1] = -1;
  EXPECT_EQ(0, pipe_pf.read_once(io_buf.data(), 4, /*blocking=*/false));
}

TEST(KduFileIoTest, FileSourcesAndTargetsCoverInlineMethods) {
  const std::filesystem::path src_path =
      std::filesystem::path(::testing::TempDir()) / "file_io_src.bin";
  const std::filesystem::path simple_tgt_path =
      std::filesystem::path(::testing::TempDir()) / "simple_tgt.bin";
  const std::filesystem::path platform_tgt_path =
      std::filesystem::path(::testing::TempDir()) / "platform_tgt.bin";
  const std::filesystem::path missing_path =
      std::filesystem::path(::testing::TempDir()) / "missing_src.bin";
  absl::Cleanup remove_files = [&] {
    std::error_code ec;
    std::filesystem::remove(src_path, ec);
    std::filesystem::remove(simple_tgt_path, ec);
    std::filesystem::remove(platform_tgt_path, ec);
  };

  {
    std::ofstream out(src_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out << "ABCDEFGH";
    ASSERT_TRUE(out.good());
  }

  VerifyFileSource<kdu_simple_file_source>(src_path, missing_path);
  VerifyFileSource<kdu_platform_file_source>(src_path, missing_path);

  kdu_membacked_file_source mem_src;
  EXPECT_FALSE(mem_src.close());
  EXPECT_EQ(nullptr, mem_src.get_membroker());
  // TODO: Upstream kdu_membacked_compressed_source destructor in
  // third_party/kakadu/vendor/coresys/compressed/codestream.cpp:4226 calls
  // `delete tmp->buffer;` instead of `delete[] tmp->buffer;` on arrays
  // allocated with `new(std::nothrow) kdu_byte[...]` at :4631, causing a
  // GWP-ASan / ASan alloc-dealloc-mismatch crash when mem_src.open() enables
  // buffering. Skip mem_src.open() and read() until upstream fixes the bug.

  VerifyFileTarget<kdu_simple_file_target>(simple_tgt_path);
  VerifyFileTarget<kdu_platform_file_target>(platform_tgt_path);
}

}  // namespace
}  // namespace kdu_supp
