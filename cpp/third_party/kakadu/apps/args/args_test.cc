// Unit tests for third_party/kakadu/vendor/apps/args/args.cpp
// and third_party/kakadu/vendor/apps/args/kdu_args.h.

#include <filesystem>  // NOLINT
#include <fstream>
#include <ios>
#include <string>
#include <system_error>  // NOLINT(build/c++11)
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/kakadu/vendor/apps/args/kdu_args.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_supp {
namespace {

class ThrowingErrorHandler final : public kdu_core::kdu_thread_safe_message {
 public:
  void put_text(const char* text) override { last_message_ += text; }
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      last_message_.clear();
      kdu_core::kdu_thread_safe_message::flush(true);
      throw KDU_ERROR_EXCEPTION;
    }
    kdu_core::kdu_thread_safe_message::flush(false);
  }

 private:
  std::string last_message_;
};

class SilentWarningHandler final : public kdu_core::kdu_thread_safe_message {};

class CapturingMessage final : public kdu_core::kdu_thread_safe_message {
 public:
  void put_text(const char* text) override { text_ += text; }
  void flush(bool end_of_message = false) override {
    ++flush_calls_;
    kdu_core::kdu_thread_safe_message::flush(end_of_message);
  }

  const std::string& text() const { return text_; }
  int flush_calls() const { return flush_calls_; }

 private:
  std::string text_;
  int flush_calls_ = 0;
};

kdu_core::kdu_message* GlobalErrorHandler() {
  static auto* const handler = new ThrowingErrorHandler();
  return handler;
}

kdu_core::kdu_message* GlobalWarningHandler() {
  static auto* const handler = new SilentWarningHandler();
  return handler;
}

class ArgsTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    kdu_core::kdu_customize_errors(GlobalErrorHandler());
    kdu_core::kdu_customize_warnings(GlobalWarningHandler());
  }
};

const ::testing::Environment* const kArgsEnv =
    ::testing::AddGlobalTestEnvironment(new ArgsTestEnvironment());

std::vector<std::string> DrainArgs(kdu_args& args) {
  std::vector<std::string> out;
  for (char* cur = args.get_first(); cur != nullptr;
       cur = args.advance(/*remove_last=*/false)) {
    out.emplace_back(cur);
  }
  return out;
}

TEST(ArgsTest, DefaultAndDegenerateConstructors) {
  kdu_args default_args;
  EXPECT_STREQ("<internal>", default_args.get_prog_name());
  EXPECT_EQ(nullptr, default_args.get_first());
  EXPECT_EQ(nullptr, default_args.find("-missing"));
  EXPECT_EQ(nullptr, default_args.advance(/*remove_last=*/true));
  EXPECT_EQ(nullptr, default_args.advance(/*remove_last=*/false));

  CapturingMessage cap;
  EXPECT_EQ(0, default_args.show_unrecognized(cap));
  EXPECT_EQ(1, cap.flush_calls());
  EXPECT_TRUE(cap.text().empty());

  char prog[] = "tool_name";
  char arg0[] = "-unused";
  char* zero_argc_argv[] = {prog, arg0};
  kdu_args zero_argc(0, zero_argc_argv);
  EXPECT_STREQ("<internal>", zero_argc.get_prog_name());
  EXPECT_EQ(nullptr, zero_argc.get_first());

  kdu_args null_argv(2, nullptr);
  EXPECT_STREQ("<internal>", null_argv.get_prog_name());
  EXPECT_EQ(nullptr, null_argv.get_first());

  char* null_prog_argv[] = {nullptr, arg0};
  kdu_args null_prog(2, null_prog_argv);
  EXPECT_STREQ("<internal>", null_prog.get_prog_name());
  EXPECT_EQ((std::vector<std::string>{"-unused"}), DrainArgs(null_prog));
}

TEST(ArgsTest, SwitchFileParsingCommentsWhitespaceAndErrors) {
  const std::filesystem::path switch_path =
      std::filesystem::path(::testing::TempDir()) / "args_switch.txt";
  absl::Cleanup remove_switch = [&switch_path] {
    std::error_code ec;
    std::filesystem::remove(switch_path, ec);
  };

  // Exercises whitespace-only lines, #/% comments, spaces/tabs/\r delimiters,
  // an embedded NUL at token start, and a final token without trailing '\n'.
  std::string switch_content =
      "   \t\r\n"
      "  # hash comment\n"
      "\t% percent comment\n"
      " -alpha\t  beta\r\n"
      "gamma  # trailing comment\r\n"
      "   ";
  switch_content.push_back('\0');
  switch_content += "ignored_after_nul\ndelta";
  {
    std::ofstream out(switch_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out.write(switch_content.data(),
              static_cast<std::streamsize>(switch_content.size()));
    ASSERT_TRUE(out.good());
  }

  char prog[] = "kakadu_app";
  char before[] = "-before";
  char sw[] = "-s";
  std::string path_str = switch_path.string();
  char after[] = "-after";
  char* argv[] = {prog, before, sw, path_str.data(), after};

  kdu_args parsed(5, argv, "-s");
  EXPECT_STREQ("kakadu_app", parsed.get_prog_name());
  EXPECT_EQ((std::vector<std::string>{"-before", "-alpha", "beta", "gamma",
                                      "delta", "-after"}),
            DrainArgs(parsed));

  // Keep argv[0] nullptr and -s first so no heap allocations precede throws.
  char* trailing_switch_argv[] = {nullptr, sw};
  EXPECT_THROW(kdu_args(2, trailing_switch_argv, "-s"),
               kdu_core::kdu_exception);

  std::string missing_path =
      (std::filesystem::path(::testing::TempDir()) / "does_not_exist.txt")
          .string();
  char* missing_file_argv[] = {nullptr, sw, missing_path.data()};
  EXPECT_THROW(kdu_args(3, missing_file_argv, "-s"), kdu_core::kdu_exception);
}

TEST(ArgsTest, AddFindAdvanceRemoveAndShowUnrecognized) {
  kdu_args args;
  args.add_arg("-one");
  args.add_arg("-two");
  args.add_arg("-three");
  // Set current = first so add_arg traverses current->next != NULL.
  EXPECT_STREQ("-one", args.get_first());
  args.add_arg("-four");

  EXPECT_EQ(nullptr, args.find("-missing"));
  // advance when current == nullptr returns nullptr safely.
  EXPECT_EQ(nullptr, args.advance(/*remove_last=*/true));

  // Remove interior node ("-two").
  EXPECT_STREQ("-two", args.find("-two"));
  EXPECT_STREQ("-three", args.advance(/*remove_last=*/true));

  // Advance without removing ("-three" -> "-four"), then remove tail ("-four").
  EXPECT_STREQ("-four", args.advance(/*remove_last=*/false));
  EXPECT_EQ(nullptr, args.advance(/*remove_last=*/true));

  // Remove head node ("-one").
  EXPECT_STREQ("-one", args.get_first());
  EXPECT_STREQ("-three", args.advance(/*remove_last=*/true));

  CapturingMessage cap;
  EXPECT_EQ(1, args.show_unrecognized(cap));
  EXPECT_EQ("Unused argument: \"-three\"\n", cap.text());
  EXPECT_EQ(1, cap.flush_calls());

  // Removing last node leaves list empty.
  EXPECT_STREQ("-three", args.find("-three"));
  EXPECT_EQ(nullptr, args.advance(/*remove_last=*/true));
  EXPECT_EQ(nullptr, args.get_first());

  CapturingMessage empty_cap;
  EXPECT_EQ(0, args.show_unrecognized(empty_cap));
  EXPECT_TRUE(empty_cap.text().empty());
}

TEST(ArgsTest, CopyConstructorDeepCopiesProgNameAndArgs) {
  kdu_args empty_src;
  kdu_args empty_copy(empty_src);
  EXPECT_STREQ("<internal>", empty_copy.get_prog_name());
  EXPECT_EQ(nullptr, empty_copy.get_first());

  char prog[] = "sample_cli";
  char a1[] = "first";
  char a2[] = "second";
  char a3[] = "third";
  char a4[] = "fourth";
  char* argv[] = {prog, a1, a2, a3, a4};
  kdu_args src(5, argv);

  // Remove one node in src; removed nodes are not copied to copy.
  ASSERT_STREQ("second", src.find("second"));
  EXPECT_STREQ("third", src.advance(/*remove_last=*/true));

  kdu_args copy(src);
  EXPECT_STREQ("sample_cli", copy.get_prog_name());
  EXPECT_NE(src.get_prog_name(), copy.get_prog_name());
  EXPECT_EQ((std::vector<std::string>{"first", "third", "fourth"}),
            DrainArgs(copy));

  // Mutating copy by removing "first" leaves src unchanged.
  ASSERT_STREQ("first", copy.get_first());
  EXPECT_STREQ("third", copy.advance(/*remove_last=*/true));
  EXPECT_EQ((std::vector<std::string>{"third", "fourth"}), DrainArgs(copy));
  EXPECT_EQ((std::vector<std::string>{"first", "third", "fourth"}),
            DrainArgs(src));
}

}  // namespace
}  // namespace kdu_supp
