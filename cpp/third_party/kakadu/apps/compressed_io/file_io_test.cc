// Copyright 2026 Google LLC

#include <atomic>
#include <filesystem>  // NOLINT
#include <fstream>
#include <string>
#include <system_error>  // NOLINT(build/c++11)
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/kakadu/vendor/apps/compressed_io/kdu_file_io.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace {

class ThrowingMessage final : public kdu_core::kdu_message {
 public:
  ~ThrowingMessage() noexcept override = default;
  void put_text(const char* /*string*/) override {}
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      throw static_cast<kdu_core::kdu_exception>(KDU_ERROR_EXCEPTION);
    }
  }
};

class SilentMessage final : public kdu_core::kdu_message {
 public:
  ~SilentMessage() noexcept override = default;
  void put_text(const char* /*string*/) override {}
};

class KakaduTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    kdu_core::kdu_customize_errors(&error_handler_);
    kdu_core::kdu_customize_warnings(&warning_handler_);
  }

 private:
  ThrowingMessage error_handler_;
  SilentMessage warning_handler_;
};

const ::testing::Environment* const kKakaduEnv =
    ::testing::AddGlobalTestEnvironment(new KakaduTestEnvironment());

std::filesystem::path MakeTempDir() {
  static std::atomic<int> counter{0};
  std::filesystem::path dir =
      std::filesystem::path(::testing::TempDir()) /
      ("kakadu_file_io_test_" + std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void WriteStringToFile(const std::filesystem::path& path,
                       const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open());
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.close();
  ASSERT_TRUE(out.good());
}

std::string ReadStringFromFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.is_open());
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

template <typename SourceT>
void ExerciseFileSourceOpen(const std::filesystem::path& dir) {
  const std::filesystem::path file_a = dir / "source_a.bin";
  const std::filesystem::path file_b = dir / "source_b.bin";
  const std::filesystem::path missing = dir / "does_not_exist.bin";
  WriteStringToFile(file_a, "ABCDEF");
  WriteStringToFile(file_b, "XYZ");

  // 1. Open with seeking allowed and default membroker.
  SourceT src;
  ASSERT_TRUE(src.open(file_a.c_str(), /*allow_seeks=*/true,
                       /*return_on_failure=*/false, nullptr));
  EXPECT_EQ(nullptr, src.get_membroker());
  EXPECT_NE(0, src.get_capabilities() & KDU_SOURCE_CAP_SEEKABLE);

  std::vector<kdu_core::kdu_byte> buf(6);
  EXPECT_EQ(6, src.read(buf.data(), 6));
  EXPECT_EQ((std::vector<kdu_core::kdu_byte>{'A', 'B', 'C', 'D', 'E', 'F'}),
            buf);

  // 2. Re-open while already open with seeking disallowed and a membroker.
  kdu_core::kdu_membroker broker;
  ASSERT_TRUE(src.open(file_b.c_str(), /*allow_seeks=*/false,
                       /*return_on_failure=*/false, &broker));
  EXPECT_EQ(&broker, src.get_membroker());
  EXPECT_EQ(0, src.get_capabilities() & KDU_SOURCE_CAP_SEEKABLE);

  std::vector<kdu_core::kdu_byte> buf_b(3);
  EXPECT_EQ(3, src.read(buf_b.data(), 3));
  EXPECT_EQ((std::vector<kdu_core::kdu_byte>{'X', 'Y', 'Z'}), buf_b);
  EXPECT_TRUE(src.close());

  // 3. Missing file with return_on_failure=true returns false and cleans up.
  EXPECT_FALSE(src.open(missing.c_str(), /*allow_seeks=*/true,
                        /*return_on_failure=*/true, &broker));
  EXPECT_EQ(nullptr, src.get_membroker());

  // 4. Empty path with return_on_failure=true returns false.
  EXPECT_FALSE(src.open("", /*allow_seeks=*/true, /*return_on_failure=*/true));

  // 5. Missing file with return_on_failure=false throws kdu_exception.
  EXPECT_THROW(src.open(missing.c_str(), /*allow_seeks=*/true,
                        /*return_on_failure=*/false),
               kdu_core::kdu_exception);
}

template <typename TargetT>
void ExerciseFileTargetOpen(const std::filesystem::path& dir) {
  const std::filesystem::path out_a = dir / "target_a.bin";
  const std::filesystem::path out_b = dir / "target_b.bin";
  const std::filesystem::path unwritable = dir / "no_such_subdir" / "out.bin";

  TargetT tgt;
  kdu_core::kdu_membroker broker;

  // 1. Create a new file and write initial payload.
  ASSERT_TRUE(tgt.open(out_a.c_str(), /*append_to_existing=*/false,
                       /*return_on_failure=*/false, &broker));
  EXPECT_EQ(&broker, tgt.get_membroker());
  const std::string hello = "hello";
  EXPECT_TRUE(
      tgt.write(reinterpret_cast<const kdu_core::kdu_byte*>(hello.data()),
                static_cast<int>(hello.size())));

  // 2. Open a second file while still open (implicitly closes the first).
  ASSERT_TRUE(tgt.open(out_b.c_str(), /*append_to_existing=*/false,
                       /*return_on_failure=*/false, nullptr));
  EXPECT_EQ(nullptr, tgt.get_membroker());
  const std::string other = "other";
  EXPECT_TRUE(
      tgt.write(reinterpret_cast<const kdu_core::kdu_byte*>(other.data()),
                static_cast<int>(other.size())));
  EXPECT_TRUE(tgt.close());
  EXPECT_EQ("hello", ReadStringFromFile(out_a));
  EXPECT_EQ("other", ReadStringFromFile(out_b));

  // 3. Append to an existing file.
  ASSERT_TRUE(tgt.open(out_a.c_str(), /*append_to_existing=*/true,
                       /*return_on_failure=*/true, &broker));
  const std::string world = "_world";
  EXPECT_TRUE(
      tgt.write(reinterpret_cast<const kdu_core::kdu_byte*>(world.data()),
                static_cast<int>(world.size())));
  EXPECT_TRUE(tgt.close());
  EXPECT_EQ("hello_world", ReadStringFromFile(out_a));

  // 4. Overwrite an existing file when append_to_existing=false.
  ASSERT_TRUE(tgt.open(out_a.c_str(), /*append_to_existing=*/false,
                       /*return_on_failure=*/true));
  const std::string reset = "reset";
  EXPECT_TRUE(
      tgt.write(reinterpret_cast<const kdu_core::kdu_byte*>(reset.data()),
                static_cast<int>(reset.size())));
  EXPECT_TRUE(tgt.close());
  EXPECT_EQ("reset", ReadStringFromFile(out_a));

  // 5. Unwritable path with return_on_failure=true vs false.
  EXPECT_FALSE(tgt.open(unwritable.c_str(), /*append_to_existing=*/false,
                        /*return_on_failure=*/true, &broker));
  EXPECT_EQ(nullptr, tgt.get_membroker());
  EXPECT_FALSE(tgt.open(unwritable.c_str(), /*append_to_existing=*/true,
                        /*return_on_failure=*/true));
  EXPECT_THROW(tgt.open(unwritable.c_str(), /*append_to_existing=*/false,
                        /*return_on_failure=*/false),
               kdu_core::kdu_exception);
}

TEST(FileIoTest, SimpleAndPlatformFileSourceOpenPaths) {
  (void)kKakaduEnv;
  const std::filesystem::path dir = MakeTempDir();
  absl::Cleanup cleanup = [&dir]() {
    std::error_code ec;  // NOLINT(build/c++11)
    std::filesystem::remove_all(dir, ec);
  };

  ExerciseFileSourceOpen<kdu_supp::kdu_simple_file_source>(dir);
  ExerciseFileSourceOpen<kdu_supp::kdu_platform_file_source>(dir);
}

TEST(FileIoTest, SimpleAndPlatformFileTargetOpenPaths) {
  const std::filesystem::path dir = MakeTempDir();
  absl::Cleanup cleanup = [&dir]() {
    std::error_code ec;  // NOLINT(build/c++11)
    std::filesystem::remove_all(dir, ec);
  };

  ExerciseFileTargetOpen<kdu_supp::kdu_simple_file_target>(dir);
  ExerciseFileTargetOpen<kdu_supp::kdu_platform_file_target>(dir);
}

TEST(FileIoTest, MembackedFileSourceMissingFilePaths) {
  const std::filesystem::path dir = MakeTempDir();
  absl::Cleanup cleanup = [&dir]() {
    std::error_code ec;  // NOLINT(build/c++11)
    std::filesystem::remove_all(dir, ec);
  };
  const std::filesystem::path missing = dir / "nonexistent.bin";

  kdu_supp::kdu_membacked_file_source mem_src;
  EXPECT_FALSE(mem_src.close());
  EXPECT_EQ(nullptr, mem_src.get_membroker());

  kdu_core::kdu_membroker broker;
  EXPECT_FALSE(mem_src.open(missing.c_str(), KDU_MEMBACKED_SOURCE_UNSERVICED,
                            1024, 0.0, nullptr, /*return_on_failure=*/true,
                            &broker));
  EXPECT_EQ(nullptr, mem_src.get_membroker());

  EXPECT_THROW(
      mem_src.open(missing.c_str(), KDU_MEMBACKED_SOURCE_UNSERVICED, 1024, 0.0,
                   nullptr, /*return_on_failure=*/false, &broker),
      kdu_core::kdu_exception);

  // TODO: Opening a valid file or stdin ("-") with
  // kdu_membacked_file_source::open() enables buffering in
  // kdu_membacked_compressed_source, whose destructor in
  // third_party/kakadu/vendor/coresys/compressed/codestream.cpp:4226 calls
  // `delete tmp->buffer;` instead of `delete[] tmp->buffer;` on arrays
  // allocated with `new(std::nothrow) kdu_byte[...]` at :4631, causing a
  // GWP-ASan / ASan alloc-dealloc-mismatch crash. Skip valid-file/stdin open()
  // coverage in file_io.cpp until upstream fixes codestream.cpp:4226.
}

}  // namespace
